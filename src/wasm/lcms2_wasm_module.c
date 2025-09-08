/**
 * Little-CMS.wasm Main Module Implementation
 * 
 * This file provides the main WASM module interface combining:
 * - Core Little-CMS functionality
 * - WASM SIMD optimizations  
 * - WASM-native filesystem patterns
 * - JavaScript integration API
 * 
 * Copyright 2025 Superstruct Ltd, New Zealand
 * Licensed under LGPL/GPL/MPL tri-license (same as lcms2)
 */

#include "lcms2.h"
#include "lcms2_wasm_simd.h"
#include "lcms2_wasm_native.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Module initialization state
static int g_module_initialized = 0;

/**
 * Initialize the Little-CMS.wasm module
 * 
 * This function must be called first to set up:
 * - WASM filesystem (if WASM-native enabled)
 * - SIMD detection and configuration
 * - Error handling context
 * 
 * Returns: 1 on success, 0 on failure
 */
EMSCRIPTEN_KEEPALIVE
int lcms2_wasm_init(void) {
    if (g_module_initialized) return 1;
    
    // Initialize core LCMS2 with WASM-compatible settings
    cmsSetLogErrorHandler(NULL);  // Use default error handler for now
    
    #ifdef LCMS2_WASM_NATIVE
    // Initialize WASM-native filesystem
    if (!lcms2_native_init_filesystem()) {
        return 0;
    }
    #endif
    
    // Check and configure SIMD support
    #ifdef LCMS2_WASM_SIMD_ENABLED
    int simd_available = lcms2_get_simd_support();
    if (simd_available) {
        // Enable all SIMD features by default
        lcms2_configure_simd(LCMS2_SIMD_ALL, 1);
    }
    #endif
    
    g_module_initialized = 1;
    return 1;
}

/**
 * Get module information and capabilities
 * 
 * Returns JSON string with module details:
 * {
 *   "version": "2.17.0",
 *   "features": {
 *     "simd": true,
 *     "native_filesystem": true,
 *     "persistent_storage": false
 *   },
 *   "performance": {
 *     "simd_speedup": 2.8
 *   }
 * }
 */
EMSCRIPTEN_KEEPALIVE
const char* lcms2_wasm_get_info(void) {
    static char info_buffer[1024];
    
    #ifdef LCMS2_WASM_SIMD_ENABLED
    int simd_support = lcms2_get_simd_support();
    lcms2_simd_metrics metrics;
    lcms2_get_simd_metrics(&metrics);
    #else
    int simd_support = 0;
    #endif
    
    #ifdef LCMS2_WASM_NATIVE
    int cache_entries;
    size_t cache_size;
    int persistent_enabled;
    int hit_rate;
    lcms2_native_get_cache_stats(&cache_entries, &cache_size, &persistent_enabled, &hit_rate);
    #else
    int cache_entries = 0;
    size_t cache_size = 0;
    int persistent_enabled = 0;
    int hit_rate = 0;
    #endif
    
    snprintf(info_buffer, sizeof(info_buffer),
        "{"
        "\"version\":\"2.17.0\","
        "\"build_date\":\"2025-01-16\","
        "\"features\":{"
            "\"simd\":%s,"
            "\"native_filesystem\":%s,"
            "\"persistent_storage\":%s"
        "},"
        "\"performance\":{"
            "\"simd_speedup\":%.2f"
        "},"
        "\"cache\":{"
            "\"entries\":%d,"
            "\"size_mb\":%.2f,"
            "\"hit_rate\":%d"
        "}"
        "}",
        simd_support ? "true" : "false",
        #ifdef LCMS2_WASM_NATIVE
        "true",
        #else
        "false",
        #endif
        persistent_enabled ? "true" : "false",
        #ifdef LCMS2_WASM_SIMD_ENABLED
        metrics.overall_speedup,
        #else
        1.0,
        #endif
        cache_entries,
        cache_size / (1024.0 * 1024.0),
        hit_rate
    );
    
    return info_buffer;
}

/**
 * High-level color transform function with WASM optimizations
 * 
 * Combines profile loading, transform creation, and SIMD-optimized processing.
 * 
 * @param input_profile_data: ICC profile data for input colorspace
 * @param input_profile_size: Size of input profile data  
 * @param output_profile_data: ICC profile data for output colorspace
 * @param output_profile_size: Size of output profile data
 * @param input_pixels: Input pixel data
 * @param output_pixels: Output pixel buffer (must be pre-allocated)
 * @param pixel_count: Number of pixels to transform
 * @param input_format: Input pixel format (TYPE_RGB_8, TYPE_RGBA_FLT, etc.)
 * @param output_format: Output pixel format
 * @param rendering_intent: INTENT_PERCEPTUAL, INTENT_RELATIVE_COLORIMETRIC, etc.
 * @param use_simd: 1 to use SIMD optimizations, 0 for scalar
 * 
 * Returns: 1 on success, 0 on failure
 */
EMSCRIPTEN_KEEPALIVE
int lcms2_wasm_transform_image(const void* input_profile_data, size_t input_profile_size,
                               const void* output_profile_data, size_t output_profile_size,
                               const void* input_pixels, void* output_pixels,
                               unsigned int pixel_count,
                               unsigned int input_format, unsigned int output_format,
                               unsigned int rendering_intent, int use_simd) {
    
    if (!lcms2_wasm_init()) return 0;
    
    if (!input_profile_data || !output_profile_data || !input_pixels || !output_pixels) {
        return 0;
    }
    
    // Load ICC profiles
    cmsHPROFILE input_profile = cmsOpenProfileFromMem(input_profile_data, input_profile_size);
    if (!input_profile) return 0;
    
    cmsHPROFILE output_profile = cmsOpenProfileFromMem(output_profile_data, output_profile_size);
    if (!output_profile) {
        cmsCloseProfile(input_profile);
        return 0;
    }
    
    // Create color transform
    cmsHTRANSFORM transform = cmsCreateTransform(
        input_profile, input_format,
        output_profile, output_format,
        rendering_intent, 0);
    
    if (!transform) {
        cmsCloseProfile(input_profile);
        cmsCloseProfile(output_profile);
        return 0;
    }
    
    // Perform transformation with optional SIMD optimization
    #ifdef LCMS2_WASM_SIMD_ENABLED
    if (use_simd && lcms2_get_simd_support()) {
        // Try SIMD-optimized path
        if (!lcms2_colorspace_convert_simd(transform, input_pixels, output_pixels, 
                                          pixel_count, input_format)) {
            // Fall back to standard transform
            cmsDoTransform(transform, input_pixels, output_pixels, pixel_count);
        }
    } else {
        cmsDoTransform(transform, input_pixels, output_pixels, pixel_count);
    }
    #else
    cmsDoTransform(transform, input_pixels, output_pixels, pixel_count);
    #endif
    
    // Cleanup
    cmsDeleteTransform(transform);
    cmsCloseProfile(input_profile);
    cmsCloseProfile(output_profile);
    
    return 1;
}

/**
 * Load standard sRGB profile
 * 
 * Returns: Profile handle or NULL on failure
 * Note: Caller must call cmsCloseProfile() to free memory
 */
EMSCRIPTEN_KEEPALIVE
cmsHPROFILE lcms2_wasm_create_srgb_profile(void) {
    return cmsCreate_sRGBProfile();
}

/**
 * Load standard Lab profile (D50)
 * 
 * Returns: Profile handle or NULL on failure
 * Note: Caller must call cmsCloseProfile() to free memory
 */
EMSCRIPTEN_KEEPALIVE
cmsHPROFILE lcms2_wasm_create_lab_profile(void) {
    return cmsCreateLab4Profile(NULL);  // Use D50 illuminant
}

/**
 * Create RGB working space profile with custom parameters
 * 
 * @param white_x: White point x coordinate
 * @param white_y: White point y coordinate  
 * @param red_x: Red primary x coordinate
 * @param red_y: Red primary y coordinate
 * @param green_x: Green primary x coordinate
 * @param green_y: Green primary y coordinate
 * @param blue_x: Blue primary x coordinate
 * @param blue_y: Blue primary y coordinate
 * @param gamma: Gamma value for tone response curve
 * 
 * Returns: Profile handle or NULL on failure
 */
EMSCRIPTEN_KEEPALIVE
cmsHPROFILE lcms2_wasm_create_rgb_profile(double white_x, double white_y,
                                          double red_x, double red_y,
                                          double green_x, double green_y,
                                          double blue_x, double blue_y,
                                          double gamma) {
    
    cmsCIExyY white_point = {white_x, white_y, 1.0};
    cmsCIExyY primaries[3] = {
        {red_x, red_y, 1.0},
        {green_x, green_y, 1.0},
        {blue_x, blue_y, 1.0}
    };
    
    cmsToneCurve* tone_curve = cmsBuildGamma(NULL, gamma);
    if (!tone_curve) return NULL;
    
    cmsToneCurve* curves[3] = {tone_curve, tone_curve, tone_curve};
    
    cmsHPROFILE profile = cmsCreateRGBProfile(&white_point, primaries, curves);
    
    cmsFreeToneCurve(tone_curve);
    
    return profile;
}

/**
 * Benchmark color transforms with SIMD comparison
 * 
 * @param profile_a_data: First test profile data
 * @param profile_a_size: Size of first profile
 * @param profile_b_data: Second test profile data  
 * @param profile_b_size: Size of second profile
 * @param test_pixels: Sample pixel data for benchmarking
 * @param pixel_count: Number of pixels in test data
 * @param iterations: Number of benchmark iterations
 * 
 * Returns: JSON string with benchmark results
 */
EMSCRIPTEN_KEEPALIVE
const char* lcms2_wasm_benchmark_transform(const void* profile_a_data, size_t profile_a_size,
                                          const void* profile_b_data, size_t profile_b_size,
                                          const void* test_pixels, size_t pixel_count,
                                          int iterations) {
    
    static char benchmark_results[2048];
    
    if (!lcms2_wasm_init()) {
        strcpy(benchmark_results, "{\"error\":\"Module initialization failed\"}");
        return benchmark_results;
    }
    
    // Load profiles
    cmsHPROFILE profile_a = cmsOpenProfileFromMem(profile_a_data, profile_a_size);
    cmsHPROFILE profile_b = cmsOpenProfileFromMem(profile_b_data, profile_b_size);
    
    if (!profile_a || !profile_b) {
        if (profile_a) cmsCloseProfile(profile_a);
        if (profile_b) cmsCloseProfile(profile_b);
        strcpy(benchmark_results, "{\"error\":\"Profile loading failed\"}");
        return benchmark_results;
    }
    
    #ifdef LCMS2_WASM_SIMD_ENABLED
    // Run SIMD benchmark
    int simd_speedup = lcms2_benchmark_simd(profile_a, profile_b, test_pixels, pixel_count, iterations);
    
    // Get detailed metrics
    lcms2_simd_metrics metrics;
    lcms2_get_simd_metrics(&metrics);
    
    snprintf(benchmark_results, sizeof(benchmark_results),
        "{"
        "\"simd_supported\":%s,"
        "\"simd_speedup\":%.2f,"
        "\"scalar_time_ms\":%.3f,"
        "\"simd_time_ms\":%.3f,"
        "\"iterations\":%d,"
        "\"pixels_per_iteration\":%zu,"
        "\"metrics\":{"
            "\"matrix_speedup\":%.2f,"
            "\"colorspace_speedup\":%.2f,"
            "\"tone_curve_speedup\":%.2f,"
            "\"overall_speedup\":%.2f"
        "}"
        "}",
        metrics.simd_supported ? "true" : "false",
        simd_speedup / 100.0,
        metrics.total_process_time * 1000.0,
        metrics.total_process_time * 1000.0 / (simd_speedup / 100.0),
        iterations,
        pixel_count,
        metrics.matrix_speedup,
        metrics.colorspace_speedup,
        metrics.tone_curve_speedup,
        metrics.overall_speedup
    );
    #else
    snprintf(benchmark_results, sizeof(benchmark_results),
        "{"
        "\"simd_supported\":false,"
        "\"simd_speedup\":1.0,"
        "\"iterations\":%d,"
        "\"pixels_per_iteration\":%zu,"
        "\"note\":\"SIMD not compiled in this build\""
        "}",
        iterations, pixel_count
    );
    #endif
    
    cmsCloseProfile(profile_a);
    cmsCloseProfile(profile_b);
    
    return benchmark_results;
}

/**
 * Get profile information as JSON
 * 
 * @param profile_data: ICC profile data
 * @param profile_size: Size of profile data
 * 
 * Returns: JSON string with profile information
 */
EMSCRIPTEN_KEEPALIVE
const char* lcms2_wasm_get_profile_info(const void* profile_data, size_t profile_size) {
    static char info_buffer[2048];
    
    if (!profile_data || profile_size == 0) {
        strcpy(info_buffer, "{\"error\":\"Invalid profile data\"}");
        return info_buffer;
    }
    
    cmsHPROFILE profile = cmsOpenProfileFromMem(profile_data, profile_size);
    if (!profile) {
        strcpy(info_buffer, "{\"error\":\"Could not parse profile\"}");
        return info_buffer;
    }
    
    char description[256] = {0};
    char copyright[256] = {0};
    char colorspace[32] = {0};
    char device_class[32] = {0};
    
    #ifdef LCMS2_WASM_NATIVE
    lcms2_native_get_profile_info(profile, "description", description, sizeof(description));
    lcms2_native_get_profile_info(profile, "copyright", copyright, sizeof(copyright));
    lcms2_native_get_profile_info(profile, "colorspace", colorspace, sizeof(colorspace));
    lcms2_native_get_profile_info(profile, "class", device_class, sizeof(device_class));
    #else
    // Basic info extraction
    cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", description, sizeof(description));
    cmsGetProfileInfoASCII(profile, cmsInfoCopyright, "en", "US", copyright, sizeof(copyright));
    
    cmsColorSpaceSignature cs = cmsGetColorSpace(profile);
    switch (cs) {
        case cmsSigRgbData: strcpy(colorspace, "RGB"); break;
        case cmsSigCmykData: strcpy(colorspace, "CMYK"); break;
        case cmsSigLabData: strcpy(colorspace, "Lab"); break;
        case cmsSigGrayData: strcpy(colorspace, "Gray"); break;
        default: strcpy(colorspace, "Unknown"); break;
    }
    
    cmsProfileClassSignature dc = cmsGetDeviceClass(profile);
    switch (dc) {
        case cmsSigInputClass: strcpy(device_class, "Input"); break;
        case cmsSigDisplayClass: strcpy(device_class, "Display"); break;
        case cmsSigOutputClass: strcpy(device_class, "Output"); break;
        case cmsSigLinkClass: strcpy(device_class, "DeviceLink"); break;
        default: strcpy(device_class, "Unknown"); break;
    }
    #endif
    
    cmsFloat64Number version = cmsGetProfileVersion(profile);
    
    // Escape quotes in strings for JSON
    // (Simple implementation - in production would use proper JSON library)
    for (char* p = description; *p; p++) if (*p == '"') *p = '\'';
    for (char* p = copyright; *p; p++) if (*p == '"') *p = '\'';
    
    snprintf(info_buffer, sizeof(info_buffer),
        "{"
        "\"description\":\"%s\","
        "\"copyright\":\"%s\","
        "\"colorspace\":\"%s\","
        "\"device_class\":\"%s\","
        "\"version\":%.2f,"
        "\"size_bytes\":%zu"
        "}",
        description, copyright, colorspace, device_class, version, profile_size
    );
    
    cmsCloseProfile(profile);
    return info_buffer;
}

/**
 * Clean up module resources
 * 
 * Call this when done with the module to free resources.
 */
EMSCRIPTEN_KEEPALIVE
void lcms2_wasm_cleanup(void) {
    #ifdef LCMS2_WASM_NATIVE
    lcms2_native_clear_cache(0);  // Clear memory cache only
    #endif
    
    g_module_initialized = 0;
}

// For compatibility with existing LCMS2 applications
// Export the core LCMS2 functions directly
EMSCRIPTEN_KEEPALIVE cmsHPROFILE cmsOpenProfileFromMem_wasm(const void* data, cmsUInt32Number size) {
    return cmsOpenProfileFromMem(data, size);
}

EMSCRIPTEN_KEEPALIVE void cmsCloseProfile_wasm(cmsHPROFILE profile) {
    cmsCloseProfile(profile);
}

EMSCRIPTEN_KEEPALIVE cmsHTRANSFORM cmsCreateTransform_wasm(cmsHPROFILE input, cmsUInt32Number input_format,
                                                          cmsHPROFILE output, cmsUInt32Number output_format,
                                                          cmsUInt32Number intent, cmsUInt32Number flags) {
    return cmsCreateTransform(input, input_format, output, output_format, intent, flags);
}

EMSCRIPTEN_KEEPALIVE void cmsDeleteTransform_wasm(cmsHTRANSFORM transform) {
    cmsDeleteTransform(transform);
}

EMSCRIPTEN_KEEPALIVE void cmsDoTransform_wasm(cmsHTRANSFORM transform, const void* input, void* output, cmsUInt32Number size) {
    cmsDoTransform(transform, input, output, size);
}