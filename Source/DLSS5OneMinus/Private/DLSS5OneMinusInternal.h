#pragma once

#include "DLSS5OneMinusStatus.h"

namespace DLSS5OneMinus
{
    void SetStatus(const FDLSS5OneMinusStatusSnapshot& Status);
    void RunNGXSnippetBootstrap();
}
