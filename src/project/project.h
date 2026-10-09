// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

#include "tape/transport.h"

enum class LoadStatus { Ok, Io, NewerVersion, BadVersion };

struct ProjectInfo {
    int version;
    float tempo;
    int tapeIndex;
    int frames;
    int sampleRate;
    int character;
    char uiMode[16];
};

void project_info_init(ProjectInfo& info);

// On NewerVersion and BadVersion, info is left unchanged.
LoadStatus project_read_info(const char* dir, ProjectInfo& info);
LoadStatus project_write(const char* dir, const ProjectInfo& info, const TapeRuntime& rt);
LoadStatus project_read_audio(const char* dir, const ProjectInfo& info, TapeRuntime& rt);
