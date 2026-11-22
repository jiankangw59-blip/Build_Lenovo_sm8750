//! Idempotent link preparation; kept std-only so the exact Android code is host-tested.
use std::fs::{self, File, OpenOptions};
use std::io::{self, ErrorKind};
use std::os::unix::fs::{MetadataExt, symlink};
use std::path::Path;

fn source_metadata(source: &Path) -> io::Result<fs::Metadata> {
    let metadata = fs::metadata(source)?;
    if !metadata.is_file() {
        return Err(io::Error::new(ErrorKind::InvalidInput, "link source is not a regular file"));
    }
    Ok(metadata)
}

fn hard_link_matches(source: &fs::Metadata, target: &Path) -> io::Result<bool> {
    match fs::symlink_metadata(target) {
        Ok(target) => Ok(target.is_file() && source.dev() == target.dev()
            && source.ino() == target.ino()),
        Err(error) if error.kind() == ErrorKind::NotFound => Ok(false),
        Err(error) => Err(error),
    }
}

fn symlink_matches(source: &Path, target: &Path) -> io::Result<bool> {
    match fs::symlink_metadata(target) {
        Ok(metadata) if metadata.file_type().is_symlink() => Ok(fs::read_link(target)? == source),
        Ok(_) => Ok(false),
        Err(error) if error.kind() == ErrorKind::NotFound => Ok(false),
        Err(error) => Err(error),
    }
}

fn lock_parent(target: &Path) -> io::Result<File> {
    let parent = target.parent().ok_or_else(|| io::Error::new(ErrorKind::InvalidInput,
        "link target has no parent"))?;
    let lock = OpenOptions::new().read(true).write(true).create(true).truncate(false)
        .open(parent.join(".ksud-asset-links.lock"))?;
    File::lock(&lock)?;
    Ok(lock)
}

fn remove_stale_target(target: &Path) -> io::Result<()> {
    match fs::remove_file(target) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == ErrorKind::NotFound => Ok(()),
        // Preserve the actual unlink failure instead of masking it with EEXIST.
        Err(error) => Err(error),
    }
}

pub fn ensure_hard_link(source: &Path, target: &Path) -> io::Result<()> {
    let metadata = source_metadata(source)?;
    // A correct link may share ksud's immutable inode. Never unlink it.
    if hard_link_matches(&metadata, target)? { return Ok(()); }
    let _guard = lock_parent(target)?;
    let metadata = source_metadata(source)?;
    if hard_link_matches(&metadata, target)? { return Ok(()); }
    remove_stale_target(target)?;
    match fs::hard_link(source, target) {
        Err(error) if error.kind() == ErrorKind::AlreadyExists => {
            if hard_link_matches(&metadata, target)? { Ok(()) } else { Err(error) }
        }
        result => result,
    }
}

pub fn ensure_symbolic_link(source: &Path, target: &Path) -> io::Result<()> {
    source_metadata(source)?;
    if symlink_matches(source, target)? { return Ok(()); }
    let _guard = lock_parent(target)?;
    if symlink_matches(source, target)? { return Ok(()); }
    remove_stale_target(target)?;
    match symlink(source, target) {
        Err(error) if error.kind() == ErrorKind::AlreadyExists => {
            if symlink_matches(source, target)? { Ok(()) } else { Err(error) }
        }
        result => result,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU64, Ordering};
    static NEXT: AtomicU64 = AtomicU64::new(0);
    struct Temp(PathBuf);
    impl Temp {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!("ksud-link-test-{}-{}",
                std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed)));
            fs::create_dir(&path).unwrap();
            Self(path)
        }
        fn source(&self) -> PathBuf {
            let path = self.0.join("ksud"); fs::write(&path, b"verified daemon").unwrap(); path
        }
    }
    impl Drop for Temp {
        fn drop(&mut self) {
            fs::set_permissions(&self.0, fs::Permissions::from_mode(0o700)).unwrap();
            fs::remove_dir_all(&self.0).unwrap();
        }
    }

    #[test]
    fn repeated_correct_hard_link_preserves_inode_and_link_count() {
        let temp = Temp::new(); let source = temp.source(); let target = temp.0.join("ksu_susfs");
        fs::hard_link(&source, &target).unwrap();
        let before = fs::symlink_metadata(&target).unwrap();
        for _ in 0..100 { ensure_hard_link(&source, &target).unwrap(); }
        let after = fs::symlink_metadata(&target).unwrap();
        assert_eq!((before.dev(), before.ino(), before.nlink()), (after.dev(), after.ino(), after.nlink()));
        assert!(!temp.0.join(".ksud-asset-links.lock").exists());
    }

    #[test]
    fn correct_links_need_no_directory_mutation() {
        let temp = Temp::new(); let source = temp.source();
        let hard = temp.0.join("ksu_susfs"); let symbolic = temp.0.join("resetprop");
        fs::hard_link(&source, &hard).unwrap(); symlink(&source, &symbolic).unwrap();
        fs::set_permissions(&temp.0, fs::Permissions::from_mode(0o500)).unwrap();
        ensure_hard_link(&source, &hard).unwrap(); ensure_symbolic_link(&source, &symbolic).unwrap();
        assert!(!temp.0.join(".ksud-asset-links.lock").exists());
    }

    #[test]
    fn missing_and_stale_hard_links_are_prepared() {
        let temp = Temp::new(); let source = temp.source(); let target = temp.0.join("ksu_susfs");
        ensure_hard_link(&source, &target).unwrap();
        fs::remove_file(&target).unwrap(); fs::write(&target, b"old daemon").unwrap();
        ensure_hard_link(&source, &target).unwrap();
        assert!(hard_link_matches(&fs::metadata(&source).unwrap(), &target).unwrap());
    }

    #[test]
    fn a_symlink_is_not_mistaken_for_a_hard_link() {
        let temp = Temp::new(); let source = temp.source(); let target = temp.0.join("ksu_susfs");
        symlink(&source, &target).unwrap(); ensure_hard_link(&source, &target).unwrap();
        assert!(fs::symlink_metadata(&target).unwrap().is_file());
    }

    #[test]
    fn correct_symbolic_link_preserves_its_inode() {
        let temp = Temp::new(); let source = temp.source(); let target = temp.0.join("resetprop");
        symlink(&source, &target).unwrap(); let inode = fs::symlink_metadata(&target).unwrap().ino();
        for _ in 0..100 { ensure_symbolic_link(&source, &target).unwrap(); }
        assert_eq!(inode, fs::symlink_metadata(&target).unwrap().ino());
    }

    #[test]
    fn missing_and_stale_symlinks_are_prepared() {
        let temp = Temp::new(); let source = temp.source(); let target = temp.0.join("resetprop");
        ensure_symbolic_link(&source, &target).unwrap(); fs::remove_file(&target).unwrap();
        symlink("/missing/old-ksud", &target).unwrap();
        ensure_symbolic_link(&source, &target).unwrap(); assert_eq!(fs::read_link(target).unwrap(), source);
    }

    #[test]
    fn removal_error_is_preserved_and_directory_contents_survive() {
        let temp = Temp::new(); let source = temp.source(); let target = temp.0.join("ksu_susfs");
        fs::create_dir(&target).unwrap(); fs::write(target.join("keep"), b"state").unwrap();
        let error = ensure_hard_link(&source, &target).unwrap_err();
        assert_ne!(error.kind(), ErrorKind::AlreadyExists);
        assert_eq!(fs::read(target.join("keep")).unwrap(), b"state");
    }

    #[test]
    fn missing_source_preserves_the_existing_target() {
        let temp = Temp::new(); let target = temp.0.join("ksu_susfs"); fs::write(&target, b"keep").unwrap();
        assert_eq!(ensure_hard_link(&temp.0.join("missing"), &target).unwrap_err().kind(), ErrorKind::NotFound);
        assert_eq!(fs::read(target).unwrap(), b"keep");
    }

    #[test]
    fn concurrent_preparation_converges_without_extra_hard_links() {
        let temp = Temp::new(); let source = temp.source(); let mut threads = Vec::new();
        for _ in 0..16 {
            let source = source.clone(); let parent = temp.0.clone();
            threads.push(std::thread::spawn(move || {
                for _ in 0..20 {
                    ensure_hard_link(&source, &parent.join("ksu_susfs")).unwrap();
                    ensure_symbolic_link(&source, &parent.join("resetprop")).unwrap();
                }
            }));
        }
        for thread in threads { thread.join().unwrap(); }
        assert_eq!(fs::metadata(source).unwrap().nlink(), 2);
    }
}
