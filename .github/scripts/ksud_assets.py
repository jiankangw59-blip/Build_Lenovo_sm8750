"""Apply a userspace-only repair without changing the paired kernel source."""
from pathlib import Path


def apply(source):
    path = source / "userspace/ksud/src/assets.rs"
    text = path.read_text(encoding="utf-8")
    old = '''        let ksu_susfs = KSU_SUSFS;
        let _ = std::fs::remove_file(ksu_susfs);
        std::fs::hard_link(DAEMON_PATH, ksu_susfs)?;'''
    new = '''        crate::asset_links::ensure_hard_link(
            std::path::Path::new(DAEMON_PATH), std::path::Path::new(KSU_SUSFS),
        ).map_err(|error| anyhow::Error::new(error).context(
            format!("Failed to prepare SUSFS companion link: {KSU_SUSFS}"),
        ))?;'''
    if text.count(old) != 1:
        raise ValueError("BakaSU SUSFS companion setup changed")
    text = text.replace(old, new, 1)
    old = '''        let resetprop_link = RESETPROP_PATH;
        let _ = std::fs::remove_file(resetprop_link);
        std::os::unix::fs::symlink("/data/adb/ksud", resetprop_link)?;'''
    new = '''        crate::asset_links::ensure_symbolic_link(
            std::path::Path::new(DAEMON_PATH), std::path::Path::new(RESETPROP_PATH),
        ).map_err(|error| anyhow::Error::new(error).context(
            format!("Failed to prepare resetprop link: {RESETPROP_PATH}"),
        ))?;'''
    if text.count(old) != 1:
        raise ValueError("BakaSU resetprop setup changed")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    main = source / "userspace/ksud/src/main.rs"
    text = main.read_text(encoding="utf-8")
    if text.count("mod assets;") != 1:
        raise ValueError("BakaSU entry module changed")
    main.write_text(text.replace("mod assets;", '#[cfg(target_os = "android")]\nmod asset_links;\nmod assets;', 1),
                    encoding="utf-8", newline="\n")
    helper = Path(__file__).with_name("ksud_asset_links.rs")
    (source / "userspace/ksud/src/asset_links.rs").write_bytes(helper.read_bytes())
    return {relative: source / relative for relative in (
        "userspace/ksud/src/assets.rs", "userspace/ksud/src/main.rs",
        "userspace/ksud/src/asset_links.rs")}
