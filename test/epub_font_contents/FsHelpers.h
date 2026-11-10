#pragma once
// Reuse only the filesystem/URI boundary adapter, not the metadata suite's ZIP
// or inflate doubles. This target must exercise both production implementations.
#include "../epub_metadata/stubs/FsHelpers.h"
