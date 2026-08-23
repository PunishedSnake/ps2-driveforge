#pragma once

// Stable host-side MagicGate entry point. Keep implementation layers separate
// so format parsing, key wrapping, signatures and verification can be audited
// independently while existing callers retain one convenient include.
#include "ps2hdd/magicgate_disk_keys.hpp"
#include "ps2hdd/magicgate_known_vectors.hpp"
#include "ps2hdd/magicgate_verify.hpp"
