#pragma once

#include <sys/types.h>

bool apply_focus_priority(pid_t pid);
bool reset_priority(pid_t pid);
int get_process_priority(pid_t pid);
