#ifndef SHELL_INSTALLER_H
#define SHELL_INSTALLER_H

#include <kernel/types.h>

void *limine_get_kernel_file(size_t *size_out);
void *limine_get_module(const char *name_sub, size_t *size_out);

int installer_run(const char *target_device);
void cmd_install(int argc, char **argv);

#endif // SHELL_INSTALLER_H
