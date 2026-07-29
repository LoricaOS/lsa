/* lsa-entry — the LSA console service (spawned by vigil as the console).
 *
 * If the kernel cmdline carries lsa.exec=<base64 command>, run it via `stsh -c`
 * between output markers, then reboot (which makes Firecracker exit) so the host
 * `lsa <cmd>` sees clean output + an exit code and returns. Otherwise open an
 * interactive shell. Compiled (not a #! script) so vigil execs it directly.
 *
 * The reboot needs CAP_KIND_POWER — granted via /etc/aegis/caps.d/lsa-entry.
 */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#define LSA_BEGIN "\n__LSA_BEGIN__\n"
#define LSA_END   "\n__LSA_END__ %d\n"

static int b64v(int c){
    if(c>='A'&&c<='Z')return c-'A';
    if(c>='a'&&c<='z')return c-'a'+26;
    if(c>='0'&&c<='9')return c-'0'+52;
    if(c=='+')return 62; if(c=='/')return 63; return -1;
}
static void b64dec(const char*in,long n,char*out){
    int bits=0; long val=0,o=0;
    for(long i=0;i<n;i++){ int v=b64v((unsigned char)in[i]); if(v<0)continue;
        val=(val<<6)|v; bits+=6; if(bits>=8){bits-=8; out[o++]=(char)((val>>bits)&0xFF);} }
    out[o]=0;
}
int main(void){
    char cl[4096]; long n=0; int fd=open("/proc/cmdline",O_RDONLY);
    if(fd>=0){ n=read(fd,cl,sizeof cl-1); close(fd);} if(n<0)n=0; cl[n]=0;
    char*p=strstr(cl,"lsa.exec=");
    if(p){
        p+=9; char*e=p; while(*e&&*e!=' '&&*e!='\n'&&*e!='\t')e++;
        static char cmd[4096]; b64dec(p,e-p,cmd);
        write(1, LSA_BEGIN, sizeof LSA_BEGIN - 1);
        pid_t pid=fork();
        if(pid==0){ putenv("PATH=/bin"); char*av[]={"/bin/stsh","-c",cmd,0};
                    execv("/bin/stsh",av); _exit(127); }
        int st=0; if(pid>0) waitpid(pid,&st,0);
        int rc = WIFEXITED(st)?WEXITSTATUS(st):1;
        dprintf(1, LSA_END, rc);
        sync();
        syscall(169,1,0,0);        /* reboot(kbd reset) -> Firecracker exits */
        _exit(rc);
    }
    char*av[]={"/bin/stsh",0}; execv("/bin/stsh",av); return 127;
}
