#pragma once

#include <stdbool.h>
#include <stdio.h>

extern bool mk1_verbose;

#define MK1_LOG(...)   do { fprintf(stderr, "mk1-linux: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define MK1_DEBUG(...) do { if (mk1_verbose) MK1_LOG(__VA_ARGS__); } while (0)
