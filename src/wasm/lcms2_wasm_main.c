/**
 * Little-CMS.wasm Main Entry Point
 * 
 * Simple main function for the WASM module build.
 * The actual functionality is provided through exported functions.
 * 
 * Copyright 2025 Superstruct Ltd, New Zealand
 * Licensed under LGPL/GPL/MPL tri-license (same as lcms2)
 */

#include "lcms2_wasm_module.c"

int main() {
    // Initialize the module
    lcms2_wasm_init();
    return 0;
}