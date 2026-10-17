#include <stdio.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>

int main(int argc, char **argv)
{
    printf("Bonjour depuis un programme C compilé pour Amiga (%d arguments)\n", argc - 1);
    for (int i = 1; i < argc; i++) printf("  argv[%d] = %s\n", i, argv[i]);
    printf("exec v%d, tâche « %s »\n", SysBase->LibNode.lib_Version, FindTask(NULL)->tc_Node.ln_Name);
    return 0;
}
