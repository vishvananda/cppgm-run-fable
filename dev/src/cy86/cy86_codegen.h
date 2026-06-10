#pragma once

#include "cy86/cy86_parser.h"
#include "x86/elf_program.h"

// PA9 CY86-to-x86-64 translation: each CY86 statement becomes one
// image item, either its data bytes or a fixed x86 instruction
// sequence (see cy86_codegen.cpp for the register and red-zone
// conventions). Labels and the entry point are bound on `image`;
// the caller writes the executable.
void TranslateCY86Program(const CY86Program& program, ProgramImage& image);
