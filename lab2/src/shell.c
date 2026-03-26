#include "sbi.h"
#include "uart.h"
#include "shell.h"
#include "bootloader.h"
#include "cpio.h"

static const char* skip_spaces(const char *s) {
    while (*s == ' ') {
        s++;
    }
    return s;
}

void print_shell_prompt(){
    uart_puts("opi-rv2>");
}

int check_command(const char *input_str, const char *cmd_str){
    while (*input_str && *cmd_str)
    {
        if(*input_str == *cmd_str){
            input_str++;
            cmd_str++;
        }
        else return 0;
    }
    return (*input_str == *cmd_str);
}

void print_help(){
    uart_puts("Available commands:\n");
    uart_puts("  help   - show all commands.\n");
    uart_puts("  hello  - print Hello world.\n");
    uart_puts("  info   - print system info.\n");
    uart_puts("  ls   - list files in initramfs.\n");
    uart_puts("  cat <file> - print file content from initramfs.\n");
    uart_puts("  load   - receive kernel_payload.bin over UART and jump to it.\n");
}

void print_hello(){
    uart_puts("Hello world.\n");
}

void print_info(){
    uart_puts("System information:\n");
    uart_puts("  OpenSBI specification version: ");
    uart_hex(sbi_get_spec_version());
    uart_puts("\n");

    uart_puts("  implementation ID: ");
    uart_hex(sbi_get_impl_id());
    uart_puts("\n");

    uart_puts("  implementation version: ");
    uart_hex(sbi_get_impl_version());
    uart_puts("\n");
}

void run_command(const char *cmd){
    if(cmd[0] == '\0') return;

    if(check_command(cmd, "hello")){
        print_hello();
    }
    else if(check_command(cmd, "help")){
        print_help();
    }
    else if(check_command(cmd, "info")){
        print_info();
    }
    else if(check_command(cmd, "ls")){
        initrd_list(NULL);
    }
    else if(cmd[0] == 'c' && cmd[1] == 'a' && cmd[2] == 't' &&
            (cmd[3] == '\0' || cmd[3] == ' ')) {
        const char *filename = skip_spaces(cmd + 3);

        if (*filename == '\0') {
            uart_puts("Usage: cat <filename>\n");
            return;
        }

        initrd_cat(NULL, filename);
    }
    else if(check_command(cmd, "load")){
        bootloader_load();
    }
    else{
        uart_puts("Unknown command: ");
        uart_puts(cmd);
        uart_puts("\n");
        uart_puts("Use help to get commands.\n");
    }
}
