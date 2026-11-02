#pragma once

#include <string>
#include <sys/types.h>

std::string get_process_name(pid_t pid);
char get_process_state(pid_t pid);
