/**
 * WASM-Native Filesystem and Profile Management Header for Little-CMS.wasm
 * 
 * This header defines the WASM-native API for advanced web integration
 * including persistent ICC profile storage, async profile loading, and CDN support.
 * 
 * Copyright 2025 Superstruct Ltd, New Zealand
 * Licensed under LGPL/GPL/MPL tri-license (same as lcms2)
 */

#ifndef LCMS2_WASM_NATIVE_H
#define LCMS2_WASM_NATIVE_H

#include "lcms2.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize WASM-native file system with virtual directories and IDBFS
 * Returns: 1 on success, 0 on failure
 */
int lcms2_native_init_filesystem(void);

/**
 * Set persistent storage availability (called from JavaScript after IDBFS sync)
 * @param available: 1 if persistent storage is available, 0 otherwise
 */
void lcms2_native_set_persistent_storage(int available);

/**
 * Load ICC profile from URL with intelligent caching
 * 
 * This function implements the WASM-native pattern for async profile loading:
 * 1. Check local cache (memory + persistent storage)
 * 2. If cache miss, download asynchronously with emscripten_async_wget
 * 3. Store in cache with automatic management (LRU eviction, size limits)
 * 4. Sync with IndexedDB for persistence across browser sessions
 * 
 * @param profile_url: URL to load ICC profile from
 * @param profile_name: Name for cache key generation (e.g., "sRGB", "Adobe_RGB")
 * @param callback: Callback function called when loading completes
 * @param user_data: User data passed to callback
 * 
 * Returns: 1 if loading initiated successfully, 0 on error
 * 
 * Callback signature: void callback(cmsHPROFILE profile, int success, void* user_data)
 * - profile: Loaded ICC profile handle (NULL on failure)
 * - success: 1 if successful, 0 if failed
 * - user_data: User data passed to lcms2_native_load_profile_from_url
 */
int lcms2_native_load_profile_from_url(const char* profile_url, const char* profile_name,
                                       void (*callback)(cmsHPROFILE profile, int success, void* user_data),
                                       void* user_data);

/**
 * Load standard color profiles from CDN providers
 * 
 * Supports loading from well-known profile sources:
 * - Adobe RGB, sRGB, ProPhoto RGB, DCI-P3, Rec.2020
 * - CMYK profiles: SWOP, GRACoL, FOGRA, Japan Color
 * - Monitor profiles: Generic LCD, CRT, Wide Gamut
 * 
 * @param provider: CDN provider name ("adobe", "eci", "icc", "google")
 * @param profile_name: Standard profile name (e.g., "sRGB", "AdobeRGB", "ProPhotoRGB")
 * @param callback: Completion callback
 * @param user_data: User data for callback
 * 
 * Returns: 1 if loading initiated, 0 on error
 */
int lcms2_native_load_standard_profile(const char* provider, const char* profile_name,
                                       void (*callback)(cmsHPROFILE profile, int success, void* user_data),
                                       void* user_data);

/**
 * Create custom workspace profile with persistent storage
 * 
 * Creates a custom working space profile that persists across browser sessions
 * using IDBFS storage. Useful for photographers and designers.
 * 
 * @param workspace_name: Name for the custom workspace
 * @param primaries: RGB primaries (x,y coordinates for R,G,B)
 * @param white_point: White point (x,y coordinates)
 * @param gamma: Tone curve gamma (e.g., 2.2, 1.8, "sRGB")
 * @param user_id: Optional user identifier for isolation (can be NULL)
 * 
 * Returns: Profile handle (cmsHPROFILE) or NULL on failure
 */
cmsHPROFILE lcms2_native_create_workspace_profile(const char* workspace_name,
                                                  const cmsCIExyY primaries[3],
                                                  const cmsCIExyY* white_point,
                                                  const char* gamma,
                                                  const char* user_id);

/**
 * Enable progressive loading for large profiles
 * 
 * Implements streaming profile loading for improved startup performance:
 * 1. Load basic profile structure first (colorimetry, curves)
 * 2. Stream additional data asynchronously (LUTs, metadata)
 * 3. Provide color transforms during loading with limited functionality
 * 
 * @param enable: 1 to enable progressive loading, 0 to disable
 * @param priority_tags: Array of tag signatures to load first
 * @param priority_count: Number of priority tags
 * 
 * Returns: 1 on success, 0 on failure
 */
int lcms2_native_set_progressive_loading(int enable, const cmsTagSignature* priority_tags, int priority_count);

/**
 * Get profile loading progress
 * 
 * @param profile: Profile handle
 * @param total_tags: Pointer to receive total tag count (can be NULL)
 * @param loaded_tags: Pointer to receive loaded tag count (can be NULL)
 * @param is_complete: Pointer to receive completion status (can be NULL)
 * 
 * Returns: Loading percentage (0-100)
 */
int lcms2_native_get_loading_progress(cmsHPROFILE profile, int* total_tags, int* loaded_tags, int* is_complete);

/**
 * Get cache statistics
 * 
 * @param entry_count: Pointer to receive number of cached entries (can be NULL)
 * @param total_size: Pointer to receive total cache size in bytes (can be NULL)
 * @param persistent_enabled: Pointer to receive persistent storage status (can be NULL)
 * @param hit_rate: Pointer to receive cache hit rate as percentage (can be NULL)
 */
void lcms2_native_get_cache_stats(int* entry_count, size_t* total_size, 
                                  int* persistent_enabled, int* hit_rate);

/**
 * Configure cache behavior
 * 
 * @param max_entries: Maximum number of cache entries (0 = unlimited)
 * @param max_size_mb: Maximum cache size in megabytes (0 = unlimited)  
 * @param ttl_hours: Time-to-live in hours for cache entries (0 = never expire)
 * 
 * Returns: 1 on success, 0 on failure
 */
int lcms2_native_configure_cache(int max_entries, int max_size_mb, int ttl_hours);

/**
 * Clear profile cache
 * 
 * @param clear_persistent: 1 to also clear persistent storage, 0 for memory only
 * 
 * Returns: 1 on success, 0 on failure
 */
int lcms2_native_clear_cache(int clear_persistent);

/**
 * Enable/disable offline mode
 * 
 * When enabled, only cached profiles are used (no network requests).
 * 
 * @param enable: 1 to enable offline mode, 0 to allow network requests
 * 
 * Returns: 1 on success, 0 on failure
 */
int lcms2_native_set_offline_mode(int enable);

/**
 * Check if profile is available offline
 * 
 * @param profile_name: Profile name to check
 * 
 * Returns: 1 if available offline, 0 if requires network
 */
int lcms2_native_is_profile_offline(const char* profile_name);

/**
 * Preload profiles for offline use
 * 
 * Downloads and caches specified profiles for offline availability.
 * 
 * @param profile_names: Array of profile names to preload
 * @param profile_count: Number of profile names
 * @param progress_callback: Optional progress callback (can be NULL)
 * @param completion_callback: Completion callback  
 * @param user_data: User data for callbacks
 * 
 * Returns: 1 if preloading initiated, 0 on error
 * 
 * Progress callback signature: void callback(const char* profile_name, int progress_percent, void* user_data)
 * Completion callback signature: void callback(int successful_count, int total_count, void* user_data)
 */
int lcms2_native_preload_profiles(const char** profile_names, int profile_count,
                                  void (*progress_callback)(const char* profile_name, int progress_percent, void* user_data),
                                  void (*completion_callback)(int successful_count, int total_count, void* user_data),
                                  void* user_data);

/**
 * Export profile to different formats
 * 
 * @param profile: Profile handle to export
 * @param format: Export format ("icc", "json", "xml", "txt")
 * @param output_buffer: Buffer to receive exported data
 * @param buffer_size: Size of output buffer
 * 
 * Returns: Number of bytes written to buffer, or negative on error
 */
int lcms2_native_export_profile(cmsHPROFILE profile, const char* format, 
                                char* output_buffer, size_t buffer_size);

/**
 * Import profile from data
 * 
 * @param format: Import format ("icc", "json", "xml")
 * @param input_data: Data to import
 * @param data_size: Size of input data
 * 
 * Returns: Profile handle or NULL on error
 */
cmsHPROFILE lcms2_native_import_profile(const char* format,
                                        const char* input_data, size_t data_size);

/**
 * Set network timeout for profile downloads
 * 
 * @param timeout_ms: Timeout in milliseconds (default: 30000)
 * 
 * Returns: 1 on success, 0 on failure
 */
int lcms2_native_set_network_timeout(int timeout_ms);

/**
 * Enable/disable compression for network transfers
 * 
 * @param enable: 1 to enable compression, 0 to disable
 * 
 * Returns: 1 on success, 0 on failure
 */
int lcms2_native_set_compression(int enable);

/**
 * Get profile metadata and information
 * 
 * @param profile: Profile handle
 * @param info_type: Type of information requested
 * @param output_buffer: Buffer for output
 * @param buffer_size: Size of output buffer
 * 
 * Info types:
 * - "description": Profile description
 * - "copyright": Copyright information
 * - "manufacturer": Profile manufacturer
 * - "model": Device model
 * - "colorspace": Color space (RGB, CMYK, Lab, etc.)
 * - "pcs": Profile Connection Space
 * - "intent": Rendering intent
 * - "illuminant": White point illuminant
 * - "size": File size in bytes
 * - "version": ICC version
 * - "class": Profile class
 * 
 * Returns: Length of information string, or -1 on error
 */
int lcms2_native_get_profile_info(cmsHPROFILE profile, const char* info_type,
                                  char* output_buffer, size_t buffer_size);

/**
 * Create transform with WASM-native optimizations
 * 
 * Creates a color transform with WASM-specific optimizations:
 * - SIMD-accelerated processing when available
 * - Web Worker compatibility for large datasets
 * - Progressive transform for streaming data
 * 
 * @param input_profile: Input color profile
 * @param input_format: Input pixel format
 * @param output_profile: Output color profile
 * @param output_format: Output pixel format
 * @param intent: Rendering intent
 * @param flags: Transform flags
 * 
 * Returns: Transform handle or NULL on error
 */
cmsHTRANSFORM lcms2_native_create_transform(cmsHPROFILE input_profile, cmsUInt32Number input_format,
                                            cmsHPROFILE output_profile, cmsUInt32Number output_format,
                                            cmsUInt32Number intent, cmsUInt32Number flags);

/**
 * Transform image data with WASM optimizations
 * 
 * @param transform: Transform handle
 * @param input_data: Input pixel data
 * @param output_data: Output pixel data
 * @param pixel_count: Number of pixels
 * @param use_simd: 1 to prefer SIMD, 0 for scalar
 * @param progress_callback: Optional progress callback for large datasets
 * @param user_data: User data for progress callback
 * 
 * Returns: 1 on success, 0 on failure
 */
int lcms2_native_transform_image(cmsHTRANSFORM transform,
                                 const void* input_data,
                                 void* output_data,
                                 cmsUInt32Number pixel_count,
                                 int use_simd,
                                 void (*progress_callback)(int percent, void* user_data),
                                 void* user_data);

/**
 * Validate profile integrity and compatibility
 * 
 * @param profile: Profile to validate
 * @param validation_level: Validation strictness (1=basic, 2=standard, 3=strict)
 * @param error_buffer: Buffer for error messages (can be NULL)
 * @param error_buffer_size: Size of error buffer
 * 
 * Returns: 1 if valid, 0 if invalid
 */
int lcms2_native_validate_profile(cmsHPROFILE profile, int validation_level,
                                  char* error_buffer, size_t error_buffer_size);

#ifdef __cplusplus
}
#endif

#endif // LCMS2_WASM_NATIVE_H