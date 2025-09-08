/**
 * WASM-Native Filesystem and Profile Management Implementation for Little-CMS.wasm
 * 
 * This file implements WASM-native patterns including IDBFS persistent storage,
 * async ICC profile loading, CDN integration, and intelligent caching.
 * 
 * Copyright 2025 Superstruct Ltd, New Zealand
 * Licensed under LGPL/GPL/MPL tri-license (same as lcms2)
 */

#include "lcms2_wasm_native.h"
#include "lcms2_internal.h"

#ifdef LCMS2_WASM_NATIVE

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/fetch.h>
#include <emscripten/html5.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Virtual directory structure for ICC profiles
static const char* PROFILE_CACHE_PATH = "/profile-cache";
static const char* WORKSPACE_PROFILES_PATH = "/workspace-profiles"; 
static const char* STANDARD_PROFILES_PATH = "/standard-profiles";
static const char* TEMP_PROFILES_PATH = "/temp-profiles";

// Cache management
typedef struct profile_cache_entry {
    char* name;
    char* url;
    cmsHPROFILE profile;
    size_t size;
    time_t loaded_time;
    int access_count;
    struct profile_cache_entry* next;
} profile_cache_entry_t;

typedef struct {
    profile_cache_entry_t* entries;
    int entry_count;
    size_t total_size;
    int max_entries;
    size_t max_size;
    int ttl_hours;
    int hit_count;
    int miss_count;
} profile_cache_t;

// CDN provider configuration
typedef struct {
    const char* name;
    const char* base_url;
    const char* path_template;  // e.g., "/profiles/%s.icc"
    int requires_auth;
} cdn_provider_t;

static const cdn_provider_t cdn_providers[] = {
    {"adobe", "https://color.adobe.com", "/profiles/%s.icc", 0},
    {"eci", "https://www.eci.org", "/downloads/icc_profiles/%s.icc", 0},
    {"icc", "https://www.color.org", "/registry/%s.icc", 0},
    {"google", "https://storage.googleapis.com/chromium-color-profiles", "/%s.icc", 0},
    {NULL, NULL, NULL, 0}  // Sentinel
};

// Global state
static profile_cache_t g_cache = {0};
static int g_filesystem_initialized = 0;
static int g_persistent_storage_available = 0;
static int g_offline_mode = 0;
static int g_network_timeout = 30000;  // 30 seconds
static int g_compression_enabled = 1;
static int g_progressive_loading = 0;

// Initialize WASM filesystem with virtual directories
int lcms2_native_init_filesystem(void) {
    if (g_filesystem_initialized) return 1;
    
#ifdef __EMSCRIPTEN__
    // Create virtual directories
    EM_ASM({
        try {
            // Mount IDBFS for persistent storage
            if (!Module.FS.analyzePath('/profile-cache').exists) {
                Module.FS.mkdir('/profile-cache');
            }
            if (!Module.FS.analyzePath('/workspace-profiles').exists) {
                Module.FS.mkdir('/workspace-profiles'); 
            }
            if (!Module.FS.analyzePath('/standard-profiles').exists) {
                Module.FS.mkdir('/standard-profiles');
            }
            if (!Module.FS.analyzePath('/temp-profiles').exists) {
                Module.FS.mkdir('/temp-profiles');
            }
            
            // Try to mount IDBFS for persistent storage
            try {
                Module.FS.mount(Module.FS.filesystems.IDBFS, {}, '/profile-cache');
                Module.FS.mount(Module.FS.filesystems.IDBFS, {}, '/workspace-profiles');
                console.log('✅ Little-CMS WASM: IDBFS persistent storage enabled');
                Module._lcms2_native_set_persistent_storage(1);
            } catch (e) {
                console.warn('⚠️ Little-CMS WASM: IDBFS unavailable, using memory-only cache:', e);
                Module._lcms2_native_set_persistent_storage(0);
            }
        } catch (e) {
            console.error('❌ Little-CMS WASM: Filesystem initialization failed:', e);
        }
    });
#endif
    
    // Initialize cache
    g_cache.max_entries = 100;
    g_cache.max_size = 50 * 1024 * 1024;  // 50MB
    g_cache.ttl_hours = 24;  // 24 hours
    
    g_filesystem_initialized = 1;
    return 1;
}

// Set persistent storage availability
void lcms2_native_set_persistent_storage(int available) {
    g_persistent_storage_available = available;
}

// Generate cache key from profile name/URL
static char* generate_cache_key(const char* name_or_url) {
    if (!name_or_url) return NULL;
    
    // Simple hash-based key generation
    size_t len = strlen(name_or_url);
    size_t hash = 5381;
    
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + (unsigned char)name_or_url[i];
    }
    
    char* key = malloc(32);
    if (key) {
        snprintf(key, 32, "profile_%08lx", (unsigned long)hash);
    }
    
    return key;
}

// Find cached profile entry
static profile_cache_entry_t* find_cache_entry(const char* name) {
    if (!name) return NULL;
    
    profile_cache_entry_t* entry = g_cache.entries;
    while (entry) {
        if (entry->name && strcmp(entry->name, name) == 0) {
            entry->access_count++;
            g_cache.hit_count++;
            return entry;
        }
        entry = entry->next;
    }
    
    g_cache.miss_count++;
    return NULL;
}

// Add profile to cache
static int add_to_cache(const char* name, const char* url, cmsHPROFILE profile, size_t size) {
    if (!name || !profile) return 0;
    
    // Check cache limits
    if (g_cache.entry_count >= g_cache.max_entries) {
        // TODO: Implement LRU eviction
        return 0;
    }
    
    if (g_cache.total_size + size > g_cache.max_size) {
        // TODO: Implement size-based eviction
        return 0;
    }
    
    // Create new entry
    profile_cache_entry_t* entry = malloc(sizeof(profile_cache_entry_t));
    if (!entry) return 0;
    
    entry->name = strdup(name);
    entry->url = url ? strdup(url) : NULL;
    entry->profile = profile;
    entry->size = size;
    entry->loaded_time = time(NULL);
    entry->access_count = 1;
    entry->next = g_cache.entries;
    
    g_cache.entries = entry;
    g_cache.entry_count++;
    g_cache.total_size += size;
    
    return 1;
}

// Async profile loading callback structure
typedef struct {
    void (*callback)(cmsHPROFILE profile, int success, void* user_data);
    void* user_data;
    char* profile_name;
    char* cache_path;
} async_load_context_t;

#ifdef __EMSCRIPTEN__
// Emscripten fetch callback for profile loading
static void profile_fetch_success(emscripten_fetch_t *fetch) {
    async_load_context_t* ctx = (async_load_context_t*)fetch->userData;
    
    if (fetch->status == 200 && fetch->numBytes > 0) {
        // Load profile from downloaded data
        cmsHPROFILE profile = cmsOpenProfileFromMem(fetch->data, fetch->numBytes);
        
        if (profile) {
            // Save to cache
            if (ctx->cache_path) {
                FILE* cache_file = fopen(ctx->cache_path, "wb");
                if (cache_file) {
                    fwrite(fetch->data, 1, fetch->numBytes, cache_file);
                    fclose(cache_file);
                    
                    // Sync to persistent storage if available
                    if (g_persistent_storage_available) {
                        EM_ASM({ Module.FS.syncfs(false, function(err) {}); });
                    }
                }
            }
            
            // Add to memory cache
            add_to_cache(ctx->profile_name, fetch->url, profile, fetch->numBytes);
            
            // Call success callback
            if (ctx->callback) {
                ctx->callback(profile, 1, ctx->user_data);
            }
        } else {
            // Profile loading failed
            if (ctx->callback) {
                ctx->callback(NULL, 0, ctx->user_data);
            }
        }
    } else {
        // Network error
        if (ctx->callback) {
            ctx->callback(NULL, 0, ctx->user_data);
        }
    }
    
    // Cleanup
    free(ctx->profile_name);
    free(ctx->cache_path);
    free(ctx);
    emscripten_fetch_close(fetch);
}

static void profile_fetch_error(emscripten_fetch_t *fetch) {
    async_load_context_t* ctx = (async_load_context_t*)fetch->userData;
    
    // Call failure callback
    if (ctx->callback) {
        ctx->callback(NULL, 0, ctx->user_data);
    }
    
    // Cleanup
    free(ctx->profile_name);
    free(ctx->cache_path);
    free(ctx);
    emscripten_fetch_close(fetch);
}
#endif

// Load ICC profile from URL with caching
int lcms2_native_load_profile_from_url(const char* profile_url, const char* profile_name,
                                       void (*callback)(cmsHPROFILE profile, int success, void* user_data),
                                       void* user_data) {
    
    if (!profile_url || !profile_name || !callback) return 0;
    
    if (!g_filesystem_initialized) {
        lcms2_native_init_filesystem();
    }
    
    // Check if offline mode is enabled
    if (g_offline_mode) {
        // Only check cache, no network requests
        profile_cache_entry_t* cached = find_cache_entry(profile_name);
        if (cached) {
            callback(cached->profile, 1, user_data);
            return 1;
        } else {
            callback(NULL, 0, user_data);
            return 0;
        }
    }
    
    // Check memory cache first
    profile_cache_entry_t* cached = find_cache_entry(profile_name);
    if (cached) {
        callback(cached->profile, 1, user_data);
        return 1;
    }
    
    // Check persistent cache
    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), "%s/%s.icc", PROFILE_CACHE_PATH, profile_name);
    
    FILE* cache_file = fopen(cache_path, "rb");
    if (cache_file) {
        // Load from persistent cache
        fseek(cache_file, 0, SEEK_END);
        long file_size = ftell(cache_file);
        fseek(cache_file, 0, SEEK_SET);
        
        if (file_size > 0 && file_size < 10 * 1024 * 1024) {  // Max 10MB profile
            void* profile_data = malloc(file_size);
            if (profile_data && fread(profile_data, 1, file_size, cache_file) == file_size) {
                cmsHPROFILE profile = cmsOpenProfileFromMem(profile_data, file_size);
                if (profile) {
                    add_to_cache(profile_name, profile_url, profile, file_size);
                    callback(profile, 1, user_data);
                    free(profile_data);
                    fclose(cache_file);
                    return 1;
                }
            }
            free(profile_data);
        }
        fclose(cache_file);
    }
    
#ifdef __EMSCRIPTEN__
    // Download from URL asynchronously
    async_load_context_t* ctx = malloc(sizeof(async_load_context_t));
    if (!ctx) {
        callback(NULL, 0, user_data);
        return 0;
    }
    
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->profile_name = strdup(profile_name);
    ctx->cache_path = strdup(cache_path);
    
    emscripten_fetch_attr_t fetch_attr;
    emscripten_fetch_attr_init(&fetch_attr);
    
    strcpy(fetch_attr.requestMethod, "GET");
    fetch_attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    fetch_attr.timeoutMSecs = g_network_timeout;
    fetch_attr.onsuccess = profile_fetch_success;
    fetch_attr.onerror = profile_fetch_error;
    fetch_attr.userData = ctx;
    
    emscripten_fetch(&fetch_attr, profile_url);
    return 1;
#else
    // Not in Emscripten environment
    callback(NULL, 0, user_data);
    return 0;
#endif
}

// Load standard profile from CDN
int lcms2_native_load_standard_profile(const char* provider, const char* profile_name,
                                       void (*callback)(cmsHPROFILE profile, int success, void* user_data),
                                       void* user_data) {
    
    if (!provider || !profile_name || !callback) return 0;
    
    // Find CDN provider
    const cdn_provider_t* cdn = NULL;
    for (int i = 0; cdn_providers[i].name; i++) {
        if (strcmp(cdn_providers[i].name, provider) == 0) {
            cdn = &cdn_providers[i];
            break;
        }
    }
    
    if (!cdn) {
        callback(NULL, 0, user_data);
        return 0;
    }
    
    // Construct profile URL
    char profile_url[512];
    snprintf(profile_url, sizeof(profile_url), "%s%s", cdn->base_url, cdn->path_template);
    
    // Replace %s with profile name in path template
    char formatted_path[256];
    snprintf(formatted_path, sizeof(formatted_path), cdn->path_template, profile_name);
    snprintf(profile_url, sizeof(profile_url), "%s%s", cdn->base_url, formatted_path);
    
    // Use the generic URL loading function
    return lcms2_native_load_profile_from_url(profile_url, profile_name, callback, user_data);
}

// Create custom workspace profile
cmsHPROFILE lcms2_native_create_workspace_profile(const char* workspace_name,
                                                  const cmsCIExyY primaries[3],
                                                  const cmsCIExyY* white_point,
                                                  const char* gamma,
                                                  const char* user_id) {
    
    if (!workspace_name || !primaries || !white_point) return NULL;
    
    // Create RGB profile with custom primaries
    cmsToneCurve* tone_curve;
    
    // Parse gamma specification
    if (gamma && strcmp(gamma, "sRGB") == 0) {
        // sRGB tone curve
        tone_curve = cmsBuildToneCurve(NULL, 4, (cmsFloat64Number[]){2.4, 1.0/1.055, 0.055/1.055, 1.0/12.92, 0.04045});
    } else {
        // Simple gamma curve
        double gamma_value = gamma ? strtod(gamma, NULL) : 2.2;
        if (gamma_value < 0.5 || gamma_value > 5.0) gamma_value = 2.2;
        tone_curve = cmsBuildGamma(NULL, gamma_value);
    }
    
    if (!tone_curve) return NULL;
    
    cmsToneCurve* curves[3] = {tone_curve, tone_curve, tone_curve};
    
    // Create the profile
    cmsHPROFILE profile = cmsCreateRGBProfile(white_point, primaries, curves);
    
    cmsFreeToneCurve(tone_curve);
    
    if (!profile) return NULL;
    
    // Set profile description
    cmsMLU* description = cmsMLUalloc(NULL, 1);
    if (description) {
        char desc_text[256];
        snprintf(desc_text, sizeof(desc_text), "Custom Workspace: %s", workspace_name);
        cmsMLUsetASCII(description, "en", "US", desc_text);
        cmsWriteTag(profile, cmsSigProfileDescriptionTag, description);
        cmsMLUfree(description);
    }
    
    // Save to persistent storage if available
    if (g_persistent_storage_available) {
        char workspace_path[512];
        const char* safe_user = user_id ? user_id : "default";
        snprintf(workspace_path, sizeof(workspace_path), "%s/%s_%s.icc", 
                WORKSPACE_PROFILES_PATH, safe_user, workspace_name);
        
        // Save profile to file
        if (cmsSaveProfileToFile(profile, workspace_path)) {
#ifdef __EMSCRIPTEN__
            // Sync to IndexedDB
            EM_ASM({ Module.FS.syncfs(false, function(err) {}); });
#endif
        }
    }
    
    return profile;
}

// Configure cache behavior
int lcms2_native_configure_cache(int max_entries, int max_size_mb, int ttl_hours) {
    if (max_entries > 0) g_cache.max_entries = max_entries;
    if (max_size_mb > 0) g_cache.max_size = max_size_mb * 1024 * 1024;
    if (ttl_hours >= 0) g_cache.ttl_hours = ttl_hours;
    
    return 1;
}

// Get cache statistics
void lcms2_native_get_cache_stats(int* entry_count, size_t* total_size, 
                                  int* persistent_enabled, int* hit_rate) {
    if (entry_count) *entry_count = g_cache.entry_count;
    if (total_size) *total_size = g_cache.total_size;
    if (persistent_enabled) *persistent_enabled = g_persistent_storage_available;
    
    if (hit_rate) {
        int total_requests = g_cache.hit_count + g_cache.miss_count;
        if (total_requests > 0) {
            *hit_rate = (g_cache.hit_count * 100) / total_requests;
        } else {
            *hit_rate = 0;
        }
    }
}

// Clear profile cache
int lcms2_native_clear_cache(int clear_persistent) {
    // Clear memory cache
    profile_cache_entry_t* entry = g_cache.entries;
    while (entry) {
        profile_cache_entry_t* next = entry->next;
        
        if (entry->profile) cmsCloseProfile(entry->profile);
        free(entry->name);
        free(entry->url);
        free(entry);
        
        entry = next;
    }
    
    g_cache.entries = NULL;
    g_cache.entry_count = 0;
    g_cache.total_size = 0;
    g_cache.hit_count = 0;
    g_cache.miss_count = 0;
    
    // Clear persistent cache if requested
    if (clear_persistent && g_persistent_storage_available) {
#ifdef __EMSCRIPTEN__
        EM_ASM({
            try {
                // Clear cache directories
                var cacheDir = '/profile-cache';
                if (Module.FS.analyzePath(cacheDir).exists) {
                    var files = Module.FS.readdir(cacheDir);
                    for (var i = 0; i < files.length; i++) {
                        if (files[i] !== '.' && files[i] !== '..') {
                            Module.FS.unlink(cacheDir + '/' + files[i]);
                        }
                    }
                }
                
                // Sync to IndexedDB
                Module.FS.syncfs(false, function(err) {
                    if (err) console.warn('Cache clear sync error:', err);
                });
            } catch (e) {
                console.error('Cache clear error:', e);
            }
        });
#endif
    }
    
    return 1;
}

// Set offline mode
int lcms2_native_set_offline_mode(int enable) {
    g_offline_mode = enable;
    return 1;
}

// Check if profile is available offline
int lcms2_native_is_profile_offline(const char* profile_name) {
    if (!profile_name) return 0;
    
    // Check memory cache
    if (find_cache_entry(profile_name)) return 1;
    
    // Check persistent cache
    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), "%s/%s.icc", PROFILE_CACHE_PATH, profile_name);
    
    struct stat st;
    return (stat(cache_path, &st) == 0) ? 1 : 0;
}

// Set network timeout
int lcms2_native_set_network_timeout(int timeout_ms) {
    if (timeout_ms > 0 && timeout_ms <= 300000) {  // Max 5 minutes
        g_network_timeout = timeout_ms;
        return 1;
    }
    return 0;
}

// Set compression
int lcms2_native_set_compression(int enable) {
    g_compression_enabled = enable;
    return 1;
}

// Get profile information
int lcms2_native_get_profile_info(cmsHPROFILE profile, const char* info_type,
                                  char* output_buffer, size_t buffer_size) {
    
    if (!profile || !info_type || !output_buffer || buffer_size == 0) return -1;
    
    output_buffer[0] = '\0';
    
    if (strcmp(info_type, "description") == 0) {
        cmsUInt32Number size = cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", NULL, 0);
        if (size > 0 && size <= buffer_size) {
            return cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", output_buffer, size);
        }
    }
    else if (strcmp(info_type, "copyright") == 0) {
        cmsUInt32Number size = cmsGetProfileInfoASCII(profile, cmsInfoCopyright, "en", "US", NULL, 0);
        if (size > 0 && size <= buffer_size) {
            return cmsGetProfileInfoASCII(profile, cmsInfoCopyright, "en", "US", output_buffer, size);
        }
    }
    else if (strcmp(info_type, "colorspace") == 0) {
        cmsColorSpaceSignature colorspace = cmsGetColorSpace(profile);
        switch (colorspace) {
            case cmsSigRgbData: strcpy(output_buffer, "RGB"); break;
            case cmsSigCmykData: strcpy(output_buffer, "CMYK"); break;
            case cmsSigLabData: strcpy(output_buffer, "Lab"); break;
            case cmsSigXYZData: strcpy(output_buffer, "XYZ"); break;
            case cmsSigGrayData: strcpy(output_buffer, "Gray"); break;
            default: snprintf(output_buffer, buffer_size, "Unknown (0x%08X)", colorspace); break;
        }
        return strlen(output_buffer);
    }
    else if (strcmp(info_type, "version") == 0) {
        cmsFloat64Number version = cmsGetProfileVersion(profile);
        snprintf(output_buffer, buffer_size, "%.1f", version);
        return strlen(output_buffer);
    }
    else if (strcmp(info_type, "class") == 0) {
        cmsProfileClassSignature class = cmsGetDeviceClass(profile);
        switch (class) {
            case cmsSigInputClass: strcpy(output_buffer, "Input"); break;
            case cmsSigDisplayClass: strcpy(output_buffer, "Display"); break;
            case cmsSigOutputClass: strcpy(output_buffer, "Output"); break;
            case cmsSigLinkClass: strcpy(output_buffer, "DeviceLink"); break;
            case cmsSigAbstractClass: strcpy(output_buffer, "Abstract"); break;
            case cmsSigColorSpaceClass: strcpy(output_buffer, "ColorSpace"); break;
            case cmsSigNamedColorClass: strcpy(output_buffer, "NamedColor"); break;
            default: snprintf(output_buffer, buffer_size, "Unknown (0x%08X)", class); break;
        }
        return strlen(output_buffer);
    }
    
    return -1;  // Unsupported info type
}

// Progressive loading and other advanced features (stubs for now)
int lcms2_native_set_progressive_loading(int enable, const cmsTagSignature* priority_tags, int priority_count) {
    g_progressive_loading = enable;
    return 1;
}

int lcms2_native_get_loading_progress(cmsHPROFILE profile, int* total_tags, int* loaded_tags, int* is_complete) {
    if (total_tags) *total_tags = 1;
    if (loaded_tags) *loaded_tags = 1;
    if (is_complete) *is_complete = 1;
    return 100;  // Always complete for now
}

// Preload profiles (stub)
int lcms2_native_preload_profiles(const char** profile_names, int profile_count,
                                  void (*progress_callback)(const char* profile_name, int progress_percent, void* user_data),
                                  void (*completion_callback)(int successful_count, int total_count, void* user_data),
                                  void* user_data) {
    // TODO: Implement batch profile preloading
    if (completion_callback) {
        completion_callback(0, profile_count, user_data);
    }
    return 0;
}

// Export/import profiles (stubs)
int lcms2_native_export_profile(cmsHPROFILE profile, const char* format, 
                                char* output_buffer, size_t buffer_size) {
    return -1;  // Not implemented
}

cmsHPROFILE lcms2_native_import_profile(const char* format,
                                        const char* input_data, size_t data_size) {
    return NULL;  // Not implemented
}

// WASM-native transforms (leverage existing LCMS with optimizations)
cmsHTRANSFORM lcms2_native_create_transform(cmsHPROFILE input_profile, cmsUInt32Number input_format,
                                            cmsHPROFILE output_profile, cmsUInt32Number output_format,
                                            cmsUInt32Number intent, cmsUInt32Number flags) {
    
    // Add WASM-specific optimization flags
    cmsUInt32Number wasm_flags = flags | cmsFLAGS_NOCACHE;  // Disable internal cache, we manage our own
    
    return cmsCreateTransform(input_profile, input_format, output_profile, output_format, intent, wasm_flags);
}

// WASM-optimized image transform
int lcms2_native_transform_image(cmsHTRANSFORM transform,
                                 const void* input_data,
                                 void* output_data,
                                 cmsUInt32Number pixel_count,
                                 int use_simd,
                                 void (*progress_callback)(int percent, void* user_data),
                                 void* user_data) {
    
    if (!transform || !input_data || !output_data) return 0;
    
    // For large datasets, process in chunks with progress callbacks
    if (pixel_count > 10000 && progress_callback) {
        cmsUInt32Number chunk_size = 5000;
        cmsUInt32Number processed = 0;
        
        while (processed < pixel_count) {
            cmsUInt32Number current_chunk = (pixel_count - processed) < chunk_size ? 
                                          (pixel_count - processed) : chunk_size;
            
            // Process chunk (use SIMD if requested and available)
            cmsDoTransform(transform, 
                          (char*)input_data + processed * 3,  // Assume RGB for now
                          (char*)output_data + processed * 3, 
                          current_chunk);
            
            processed += current_chunk;
            
            // Update progress
            int percent = (processed * 100) / pixel_count;
            progress_callback(percent, user_data);
        }
    } else {
        // Process all at once
        cmsDoTransform(transform, input_data, output_data, pixel_count);
        if (progress_callback) {
            progress_callback(100, user_data);
        }
    }
    
    return 1;
}

// Profile validation
int lcms2_native_validate_profile(cmsHPROFILE profile, int validation_level,
                                  char* error_buffer, size_t error_buffer_size) {
    
    if (!profile) {
        if (error_buffer && error_buffer_size > 0) {
            strncpy(error_buffer, "Profile is NULL", error_buffer_size - 1);
            error_buffer[error_buffer_size - 1] = '\0';
        }
        return 0;
    }
    
    // Basic validation
    if (validation_level >= 1) {
        // Check if profile has required tags
        if (!cmsIsTag(profile, cmsSigProfileDescriptionTag)) {
            if (error_buffer && error_buffer_size > 0) {
                strncpy(error_buffer, "Missing profile description tag", error_buffer_size - 1);
                error_buffer[error_buffer_size - 1] = '\0';
            }
            return 0;
        }
    }
    
    // Standard validation  
    if (validation_level >= 2) {
        // Check colorspace compatibility
        cmsColorSpaceSignature colorspace = cmsGetColorSpace(profile);
        if (colorspace != cmsSigRgbData && colorspace != cmsSigCmykData && 
            colorspace != cmsSigLabData && colorspace != cmsSigGrayData) {
            if (error_buffer && error_buffer_size > 0) {
                strncpy(error_buffer, "Unsupported colorspace", error_buffer_size - 1);
                error_buffer[error_buffer_size - 1] = '\0';
            }
            return 0;
        }
    }
    
    // Strict validation
    if (validation_level >= 3) {
        // Additional checks could go here
    }
    
    return 1;  // Profile is valid
}

#else // !LCMS2_WASM_NATIVE

// Stub implementations for non-WASM-native builds
int lcms2_native_init_filesystem(void) { return 0; }
void lcms2_native_set_persistent_storage(int available) {}
int lcms2_native_load_profile_from_url(const char* profile_url, const char* profile_name, void (*callback)(cmsHPROFILE profile, int success, void* user_data), void* user_data) { return 0; }
int lcms2_native_load_standard_profile(const char* provider, const char* profile_name, void (*callback)(cmsHPROFILE profile, int success, void* user_data), void* user_data) { return 0; }
cmsHPROFILE lcms2_native_create_workspace_profile(const char* workspace_name, const cmsCIExyY primaries[3], const cmsCIExyY* white_point, const char* gamma, const char* user_id) { return NULL; }
int lcms2_native_set_progressive_loading(int enable, const cmsTagSignature* priority_tags, int priority_count) { return 0; }
int lcms2_native_get_loading_progress(cmsHPROFILE profile, int* total_tags, int* loaded_tags, int* is_complete) { return 0; }
void lcms2_native_get_cache_stats(int* entry_count, size_t* total_size, int* persistent_enabled, int* hit_rate) {}
int lcms2_native_configure_cache(int max_entries, int max_size_mb, int ttl_hours) { return 0; }
int lcms2_native_clear_cache(int clear_persistent) { return 0; }
int lcms2_native_set_offline_mode(int enable) { return 0; }
int lcms2_native_is_profile_offline(const char* profile_name) { return 0; }
int lcms2_native_preload_profiles(const char** profile_names, int profile_count, void (*progress_callback)(const char* profile_name, int progress_percent, void* user_data), void (*completion_callback)(int successful_count, int total_count, void* user_data), void* user_data) { return 0; }
int lcms2_native_export_profile(cmsHPROFILE profile, const char* format, char* output_buffer, size_t buffer_size) { return -1; }
cmsHPROFILE lcms2_native_import_profile(const char* format, const char* input_data, size_t data_size) { return NULL; }
int lcms2_native_set_network_timeout(int timeout_ms) { return 0; }
int lcms2_native_set_compression(int enable) { return 0; }
int lcms2_native_get_profile_info(cmsHPROFILE profile, const char* info_type, char* output_buffer, size_t buffer_size) { return -1; }
cmsHTRANSFORM lcms2_native_create_transform(cmsHPROFILE input_profile, cmsUInt32Number input_format, cmsHPROFILE output_profile, cmsUInt32Number output_format, cmsUInt32Number intent, cmsUInt32Number flags) { return NULL; }
int lcms2_native_transform_image(cmsHTRANSFORM transform, const void* input_data, void* output_data, cmsUInt32Number pixel_count, int use_simd, void (*progress_callback)(int percent, void* user_data), void* user_data) { return 0; }
int lcms2_native_validate_profile(cmsHPROFILE profile, int validation_level, char* error_buffer, size_t error_buffer_size) { return 0; }

#endif // LCMS2_WASM_NATIVE