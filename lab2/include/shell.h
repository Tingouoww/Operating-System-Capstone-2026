#ifndef SHELL_H
#define SHELL_H

void print_shell_prompt(void);
int check_command(const char *input_str, const char *cmd_str);
void print_help(void);
void print_hello(void);
void print_info(void);
void run_command(const char *cmd);


#endif
