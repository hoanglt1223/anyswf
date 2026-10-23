/* Code table for players built without compiled ActionScript (assets only). */
#include "swfrt/avm1.h"

static swf_str *no_atoms[1];
static const char *const no_utf8[1] = {""};
static const uint32_t no_bytes[1] = {0};
static const swf_code_fn no_scripts[1] = {0};

const swf_game_code SWF_GAME_CODE = {SWF_CODE_ABI, 0, 0, no_utf8, no_bytes, no_atoms, 0, no_scripts};
