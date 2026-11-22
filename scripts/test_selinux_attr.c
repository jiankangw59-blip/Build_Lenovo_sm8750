/* Execute the actual patched LSM function with observable permission/credential stand-ins. */
typedef unsigned int u32;
typedef unsigned long size_t;
typedef _Bool bool;
#define true 1
#define false 0
#define NULL ((void *)0)
#define EINVAL 22
#define EACCES 13
#define ENOMEM 12
#define GFP_KERNEL 0
#define GFP_ATOMIC 0
#define AUDIT_SELINUX_ERR 0
#define SECCLASS_PROCESS 1
#define SECCLASS_KEY 2
#define PROCESS__SETEXEC 1
#define PROCESS__SETFSCREATE 2
#define PROCESS__SETKEYCREATE 4
#define PROCESS__SETSOCKCREATE 8
#define PROCESS__SETCURRENT 16
#define PROCESS__DYNTRANSITION 32
#define PROCESS__PTRACE 64
#define KEY__CREATE 128
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
struct task_security_struct { u32 sid, exec_sid, create_sid, keycreate_sid, sockcreate_sid; };
struct cred { struct task_security_struct security; };
struct audit_buffer { int unused; };
static struct cred next;
static unsigned checks, parses, prepares, commits, aborts;
static int permission_error;
static bool oom;
static int strcmp(const char *a,const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a-(unsigned char)*b; }
static size_t length(const char *s) { size_t n=0; while(s[n])n++; return n; }
static void copy(char *d,const char *s) { while((*d++=*s++)){} }
static u32 current_sid(void) { return 1; }
static int avc_has_perm(u32 a,u32 b,u32 c,u32 d,void *e) { (void)a;(void)b;(void)c;(void)d;(void)e; checks++;return permission_error; }
static int security_context_to_sid_user(const void *v,size_t n,u32 *sid,int gfp) {
    (void)gfp;parses++;
    if(n==length("u:r:app:s0") && !strcmp(v,"u:r:app:s0")){*sid=2;return 0;}
    return -EINVAL;
}
static int security_context_to_sid_force(const void *v,size_t n,u32 *sid) {return security_context_to_sid_user(v,n,sid,0);}
static bool has_cap_mac_admin(bool audit) {(void)audit;return false;}
static void *audit_context(void) {return NULL;}
static struct audit_buffer *audit_log_start(void *c,int g,int t) {(void)c;(void)g;(void)t;return NULL;}
static void audit_log_format(struct audit_buffer *a,const char *f,...) {(void)a;(void)f;}
static void audit_log_n_untrustedstring(struct audit_buffer *a,const char *s,size_t n) {(void)a;(void)s;(void)n;}
static void audit_log_end(struct audit_buffer *a) {(void)a;}
static struct cred *prepare_creds(void) {prepares++;return oom?NULL:&next;}
static struct task_security_struct *selinux_cred(struct cred *c) {return &c->security;}
static bool current_is_single_threaded(void) {return true;}
static int security_bounded_transition(u32 a,u32 b) {(void)a;(void)b;return 0;}
static u32 ptrace_parent_sid(void) {return 0;}
static int commit_creds(struct cred *c) {(void)c;commits++;return 0;}
static void abort_creds(struct cred *c) {(void)c;aborts++;}
static void reset(void) {checks=parses=prepares=commits=aborts=0;permission_error=0;oom=false;next.security.sid=1;}

/* ACTUAL_SELINUX_SETPROCATTR */

int test_invalid_current_uses_parser_without_credentials(void) {
    char a[80],b[80];copy(a,"u:r:missing:s0");copy(b,"\n:r:missing:s0");
    reset();CHECK(selinux_setprocattr("current",a,length(a))==-EINVAL);
    CHECK(checks==1 && parses==1 && prepares==0 && commits==0 && aborts==0);
    reset();CHECK(selinux_setprocattr("current",b,length(b))==-EINVAL);
    CHECK(checks==1 && parses==1 && prepares==0 && commits==0 && aborts==0);return 0;
}
int test_permission_check_precedes_both_invalid_parses(void) {
    char text[80];const char *values[]={"u:r:missing:s0","\n:r:missing:s0"};
    for(unsigned i=0;i<2;i++){reset();permission_error=-EACCES;copy(text,values[i]);
      CHECK(selinux_setprocattr("current",text,length(text))==-EACCES);
      CHECK(checks==1 && parses==0 && prepares==0);}return 0;
}
int test_empty_current_does_not_allocate_credentials(void) {
    char empty[1]={0};reset();oom=true;
    CHECK(selinux_setprocattr("current",empty,0)==-EINVAL);
    CHECK(checks==1 && parses==0 && prepares==0);return 0;
}
int test_other_attributes_retain_newline_clear(void) {
    const char *names[]={"exec","fscreate","keycreate","sockcreate"};
    for(unsigned i=0;i<4;i++){char newline[2]={'\n',0};reset();
      CHECK(selinux_setprocattr(names[i],newline,1)==1);
      CHECK(checks==1 && parses==0 && prepares==1 && commits==1 && aborts==0);}return 0;
}
int test_valid_current_retains_transition_and_commit(void) {
    char text[80];copy(text,"u:r:app:s0");reset();
    CHECK(selinux_setprocattr("current",text,length(text))==(int)length(text));
    CHECK(checks==2 && parses==1 && prepares==1 && commits==1 && aborts==0 && next.security.sid==2);return 0;
}
int test_valid_current_retains_credential_oom_error(void) {
    char text[80];copy(text,"u:r:app:s0");reset();oom=true;
    CHECK(selinux_setprocattr("current",text,length(text))==-ENOMEM);
    CHECK(checks==1 && parses==1 && prepares==1 && commits==0);return 0;
}
