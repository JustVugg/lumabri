/* Build-time native C utility: ship web assets inside the relocatable binary.
 * No Python, npm, CDN, runtime filesystem lookup or network is required. */
#include <stdio.h>

int main(int argc, char **argv) {
    const char *names[]={"lmb_web_html","lmb_web_css","lmb_web_js","lmb_web_logo"};
    if (argc!=5) return 1;
    puts("/* Generated from src/web and logo.svg; do not edit. */");
    for (unsigned i=0; i<4; i++) {
        FILE *file=fopen(argv[i+1],"rb"); if (!file) { perror(argv[i+1]); return 1; }
        printf("static const unsigned char %s[]={\n",names[i]);
        unsigned count=0; int byte;
        while ((byte=fgetc(file))!=EOF) {
            printf("%u,",(unsigned)byte);
            if (++count%24==0) putchar('\n');
        }
        int bad=ferror(file); fclose(file); if (bad || !count) return 1;
        puts("0};");
    }
    return fflush(stdout) || ferror(stdout) ? 1 : 0;
}
