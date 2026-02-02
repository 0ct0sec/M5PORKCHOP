// SD card formatting implementation
// Reliability-hardened version with retry logic and dual FAT tables

#include "sd_format.h"
#include "config.h"
#include "sd_layout.h"
#include "sdlog.h"
#include "../web/fileserver.h"
#include <SD.h>
#include <esp_task_wdt.h>

// FATFS (ESP-IDF) headers may not exist in all Arduino builds.
// Guard format APIs to avoid compile errors.
#if __has_include(<ff.h>)
#include <ff.h>
#include <diskio.h>
#include <sd_diskio.h>
#define SD_FORMAT_HAS_FF 1
#else
#define SD_FORMAT_HAS_FF 0
#endif

namespace {

// ============================================================================
// RELIABILITY CONSTANTS
// ============================================================================
constexpr uint8_t  kMaxWriteRetries = 3;       // Retries per sector chunk
constexpr uint16_t kRetryDelayMs = 10;         // Delay between retries
constexpr uint32_t kWdtResetInterval = 100;    // Reset WDT every N chunks
constexpr uint8_t  kMaxRemountRetries = 3;     // Retries for SD remount
constexpr uint32_t kRemountBaseDelayMs = 80;   // Base delay for remount
constexpr uint32_t kSyncSettleMs = 50;         // Settle time after CTRL_SYNC
constexpr uint32_t kSpeedSampleChunks = 10;    // Chunks to sample for speed estimate
constexpr uint32_t kProgressIntervalBytes = 512 * 1024;  // Update UI every 512KB (not per-percent)

// ============================================================================
// ERASE PROGRESS TRACKING (static, no heap)
// ============================================================================
struct EraseProgress {
    uint32_t startMs;
    uint32_t bytesPerSecond;
    char stageWithEta[24];  // "ERASING ~MM:SS"
};
static EraseProgress eraseProgress;

void formatTimeRemaining(uint32_t seconds, char* buf, size_t bufSize) {
    if (seconds < 60) {
        snprintf(buf, bufSize, "~%lus", (unsigned long)seconds);
    } else if (seconds < 3600) {
        snprintf(buf, bufSize, "~%lu:%02lu", 
                 (unsigned long)(seconds / 60), 
                 (unsigned long)(seconds % 60));
    } else {
        snprintf(buf, bufSize, "~%luh%02lum", 
                 (unsigned long)(seconds / 3600), 
                 (unsigned long)((seconds % 3600) / 60));
    }
}

SDFormat::Result makeResult(bool success, bool usedFallback, const char* msg) {
    SDFormat::Result res{};
    res.success = success;
    res.usedFallback = usedFallback;
    if (msg && msg[0]) {
        strncpy(res.message, msg, sizeof(res.message) - 1);
        res.message[sizeof(res.message) - 1] = '\0';
    } else {
        res.message[0] = '\0';
    }
    return res;
}

bool wipePorkchopLayout() {
    const char* root = SDLayout::newRoot();
    if (SD.exists(root)) {
        if (!FileServer::deletePathRecursive(String(root))) {
            return false;
        }
    }
    SDLayout::setUseNewLayout(true);
    SDLayout::ensureDirs();
    return true;
}

#if SD_FORMAT_HAS_FF
constexpr uint64_t kGiB = 1024ULL * 1024 * 1024;
constexpr uint64_t kMaxFormatBytes = 32ULL * kGiB; // Cardputer docs prefer FAT32 <= 32GB

DWORD pickAllocationUnitBytes(uint64_t cardBytes) {
    if (cardBytes == 0) return 0;

    uint64_t capped = cardBytes;
    if (capped > kMaxFormatBytes) {
        capped = kMaxFormatBytes;
    }

    // Standard FAT32 allocation unit recommendations
    if (capped <= 8 * kGiB) return 4 * 1024;
    if (capped <= 16 * kGiB) return 8 * 1024;
    if (capped <= 32 * kGiB) return 16 * 1024;
    return 32 * 1024;
}

struct DiskGeometry {
    DWORD sectorSize = 0;
    DWORD sectorCount = 0;
    uint64_t bytes = 0;
};

enum class RawInitStatus : uint8_t {
    OK,
    NO_CARD,
    WRITE_PROTECT
};

void reportProgress(SDFormat::ProgressCallback cb, const char* stage, uint8_t percent) {
    if (cb) {
        cb(stage, percent);
    }
}

RawInitStatus initRawDisk(uint8_t& pdrv) {
    const uint32_t speeds[] = {
        25000000,
        20000000,
        10000000,
        8000000,
        4000000,
        1000000
    };

    Config::prepareSDBus();
    for (uint32_t speed : speeds) {
        uint8_t drive = sdcard_init(Config::sdCsPin(), &Config::sdSpi(), speed);
        if (drive == 0xFF) {
            continue;
        }
        DSTATUS status = disk_initialize(drive);
        if (status & STA_PROTECT) {
            sdcard_uninit(drive);
            return RawInitStatus::WRITE_PROTECT;
        }
        if (status & STA_NOINIT) {
            sdcard_uninit(drive);
            continue;
        }
        pdrv = drive;
        return RawInitStatus::OK;
    }
    return RawInitStatus::NO_CARD;
}

bool getDiskGeometry(uint8_t pdrv, DiskGeometry& geo) {
    DWORD sectorSize = 0;
    DWORD sectorCount = 0;
    if (disk_ioctl(pdrv, GET_SECTOR_SIZE, &sectorSize) != RES_OK) return false;
    if (disk_ioctl(pdrv, GET_SECTOR_COUNT, &sectorCount) != RES_OK) return false;
    if (sectorSize == 0 || sectorCount == 0) return false;
    geo.sectorSize = sectorSize;
    geo.sectorCount = sectorCount;
    geo.bytes = static_cast<uint64_t>(sectorSize) * sectorCount;
    return true;
}

#if FF_MULTI_PARTITION
bool partitionToMax32GiB(uint8_t pdrv, DWORD sectorSize, uint64_t cardBytes, uint8_t* workbuf, size_t workbufSize) {
    if (cardBytes <= kMaxFormatBytes) return true;
    if (!workbuf || workbufSize < FF_MAX_SS) return false;

    if (sectorSize == 0) return false;

    const uint64_t targetBytes = kMaxFormatBytes;
    const DWORD targetSectors = static_cast<DWORD>(targetBytes / sectorSize);
    if (targetSectors == 0) return false;

    DWORD partSizes[4] = {targetSectors, 0, 0, 0};
    FRESULT fr = f_fdisk(pdrv, partSizes, workbuf);
    return fr == FR_OK;
}
#endif

// Write sectors with retry logic for transient failures
bool writeWithRetry(uint8_t pdrv, const uint8_t* buf, DWORD sector, UINT count) {
    for (uint8_t attempt = 0; attempt < kMaxWriteRetries; attempt++) {
        DRESULT res = disk_write(pdrv, buf, sector, count);
        if (res == RES_OK) {
            return true;
        }
        // Transient failure - wait and retry
        delay(kRetryDelayMs);
        yield();
    }
    return false;  // All retries exhausted
}

bool fullErase(uint8_t pdrv, const DiskGeometry& geo, SDFormat::ProgressCallback cb) {
    if (geo.sectorSize == 0 || geo.sectorCount == 0) return false;

    uint64_t targetSectors = geo.sectorCount;
    const uint64_t maxSectors = kMaxFormatBytes / geo.sectorSize;
    if (maxSectors > 0 && targetSectors > maxSectors) {
        targetSectors = maxSectors;
    }
    if (targetSectors == 0) return false;

    // ========================================================================
    // PHASE 1: Try hardware TRIM/ERASE (10-100x faster if supported)
    // ========================================================================
#ifdef CTRL_TRIM
    reportProgress(cb, "TRIM", 0);
    DWORD trimRange[2] = {0, static_cast<DWORD>(targetSectors - 1)};
    DRESULT trimRes = disk_ioctl(pdrv, CTRL_TRIM, trimRange);
    if (trimRes == RES_OK) {
        // Hardware erase succeeded - fast path!
        reportProgress(cb, "TRIM", 100);
        disk_ioctl(pdrv, CTRL_SYNC, nullptr);
        delay(kSyncSettleMs);
        return true;
    }
    // TRIM not supported or failed - fall back to zero-fill
#endif

    // ========================================================================
    // PHASE 2: Zero-fill fallback with ETA tracking
    // ========================================================================
    
    // Initialize progress tracking
    eraseProgress.startMs = millis();
    eraseProgress.bytesPerSecond = 0;
    eraseProgress.stageWithEta[0] = '\0';

    // Erase buffer - explicitly zero to avoid stale data
    static uint8_t zeroBuf[4096];
    memset(zeroBuf, 0, sizeof(zeroBuf));

    const uint32_t sectorsPerChunk = static_cast<uint32_t>(sizeof(zeroBuf) / geo.sectorSize);
    if (sectorsPerChunk == 0) return false;

    uint64_t written = 0;
    uint8_t lastPercent = 255;
    uint32_t chunkCount = 0;
    uint64_t lastProgressBytes = 0;  // Track bytes for interval-based updates
    
    // Immediate feedback - show 0% right away
    reportProgress(cb, "ERASING", 0);

    while (written < targetSectors) {
        uint32_t todo = sectorsPerChunk;
        if (written + todo > targetSectors) {
            todo = static_cast<uint32_t>(targetSectors - written);
        }

        // Write with retry logic for reliability
        if (!writeWithRetry(pdrv, zeroBuf, static_cast<DWORD>(written), static_cast<UINT>(todo))) {
            // All retries failed - report failure
            return false;
        }

        written += todo;
        chunkCount++;
        
        uint64_t writtenBytes = written * geo.sectorSize;

        // Calculate write speed after sampling period
        if (chunkCount == kSpeedSampleChunks) {
            uint32_t elapsedMs = millis() - eraseProgress.startMs;
            if (elapsedMs > 0) {
                eraseProgress.bytesPerSecond = static_cast<uint32_t>((writtenBytes * 1000ULL) / elapsedMs);
            }
        }

        // Progress update: every 512KB OR when percent changes (whichever is more frequent)
        // This prevents UI freeze during long operations
        uint8_t percent = static_cast<uint8_t>((written * 100) / targetSectors);
        bool intervalUpdate = (writtenBytes - lastProgressBytes) >= kProgressIntervalBytes;
        bool percentUpdate = (percent != lastPercent);
        
        if (intervalUpdate || percentUpdate) {
            lastPercent = percent;
            lastProgressBytes = writtenBytes;
            
            // Build stage string with ETA if we have speed data
            if (eraseProgress.bytesPerSecond > 0 && written < targetSectors) {
                uint64_t bytesRemaining = (targetSectors - written) * geo.sectorSize;
                uint32_t secondsRemaining = static_cast<uint32_t>(bytesRemaining / eraseProgress.bytesPerSecond);
                
                char etaBuf[12];
                formatTimeRemaining(secondsRemaining, etaBuf, sizeof(etaBuf));
                snprintf(eraseProgress.stageWithEta, sizeof(eraseProgress.stageWithEta), 
                         "ERASE %s", etaBuf);
                reportProgress(cb, eraseProgress.stageWithEta, percent);
            } else {
                reportProgress(cb, "ERASING", percent);
            }
        }

        // Prevent watchdog timeout during long operations
        yield();
        if ((chunkCount % kWdtResetInterval) == 0) {
            esp_task_wdt_reset();
        }
    }

    // Ensure all writes are flushed to card
    disk_ioctl(pdrv, CTRL_SYNC, nullptr);
    delay(kSyncSettleMs);  // Allow card controller to settle

    return true;
}

bool fatfsFormat(uint8_t pdrv, uint64_t cardBytes, DWORD sectorSize) {
    // Cap card size to FAT32 practical limit if not partitioning
    uint64_t effectiveBytes = cardBytes;
    if (effectiveBytes > kMaxFormatBytes) {
        effectiveBytes = kMaxFormatBytes;
    }

    // FATFS format uses logical drive strings like "0:"
    char drive[3] = {static_cast<char>('0' + pdrv), ':', '\0'};
    DWORD auSize = pickAllocationUnitBytes(effectiveBytes);

#if defined(MKFS_PARM)
    MKFS_PARM opt{};
    opt.fmt = FM_FAT32;
    opt.n_fat = 2;      // DUAL FAT TABLES for redundancy (critical fix!)
    opt.align = 0;      // Auto-align to card erase block
    opt.n_root = 0;     // Default root directory entries
    opt.au_size = auSize;
#else
    // Older FatFs uses a BYTE for format flags (FDISK not supported here).
    BYTE opt = FM_FAT32;
#endif

    static uint8_t workbuf[4096];
    memset(workbuf, 0, sizeof(workbuf));

#if FF_MULTI_PARTITION
    if (cardBytes > kMaxFormatBytes) {
        if (!partitionToMax32GiB(pdrv, sectorSize, cardBytes, workbuf, sizeof(workbuf))) {
            return false;
        }
        // Re-zero workbuf after partitioning
        memset(workbuf, 0, sizeof(workbuf));
    }
#endif

    // Reset WDT before blocking f_mkfs call (can take several seconds)
    esp_task_wdt_reset();

#if defined(MKFS_PARM)
    FRESULT fr = f_mkfs(drive, &opt, workbuf, sizeof(workbuf));
#else
    FRESULT fr = f_mkfs(drive, opt, auSize, workbuf, sizeof(workbuf));
#endif

    // Reset WDT after mkfs completes
    esp_task_wdt_reset();

    return fr == FR_OK;
}

// Attempt SD remount with exponential backoff
bool remountWithRetry() {
    for (uint8_t attempt = 0; attempt < kMaxRemountRetries; attempt++) {
        uint32_t delayMs = kRemountBaseDelayMs << attempt;  // 80, 160, 320ms
        delay(delayMs);

        if (Config::reinitSD()) {
            return true;
        }

        yield();
    }
    return false;
}

#endif // SD_FORMAT_HAS_FF
} // namespace

namespace SDFormat {

Result formatCard(FormatMode mode, bool allowFallback, ProgressCallback cb) {
    bool logWasEnabled = SDLog::isEnabled();
    SDLog::close();
    SDLog::setEnabled(false);

#if SD_FORMAT_HAS_FF
    SD.end();

    uint8_t pdrv = 0xFF;
    RawInitStatus rawInit = initRawDisk(pdrv);
    if (rawInit == RawInitStatus::WRITE_PROTECT) {
        SDLog::setEnabled(logWasEnabled);
        return makeResult(false, false, "WRITE PROTECT");
    }
    if (rawInit != RawInitStatus::OK) {
        SDLog::setEnabled(logWasEnabled);
        return makeResult(false, false, "NO SD CARD");
    }

    DiskGeometry geo{};
    if (!getDiskGeometry(pdrv, geo)) {
        sdcard_uninit(pdrv);
        SDLog::setEnabled(logWasEnabled);
        return makeResult(false, false, "GEOMETRY FAIL");
    }

    if (mode == FormatMode::FULL) {
        reportProgress(cb, "ERASING", 0);
        if (!fullErase(pdrv, geo, cb)) {
            sdcard_uninit(pdrv);
            SDLog::setEnabled(logWasEnabled);
            return makeResult(false, false, "ERASE FAIL");
        }
    }

    reportProgress(cb, "FORMAT", 0);
    if (!fatfsFormat(pdrv, geo.bytes, geo.sectorSize)) {
        sdcard_uninit(pdrv);
        if (allowFallback && Config::reinitSD() && wipePorkchopLayout()) {
            reportProgress(cb, "WIPE", 100);
            SDLog::setEnabled(logWasEnabled);
            return makeResult(true, true, "WIPE OK");
        }
        SDLog::setEnabled(logWasEnabled);
        return makeResult(false, allowFallback, "FORMAT FAIL");
    }

    sdcard_uninit(pdrv);

    // Remount with retry and exponential backoff
    if (!remountWithRetry()) {
        SDLog::setEnabled(logWasEnabled);
        return makeResult(false, false, "REMOUNT FAIL");
    }

    SDLayout::setUseNewLayout(true);
    SDLayout::ensureDirs();
    reportProgress(cb, "FORMAT", 100);
    SDLog::setEnabled(logWasEnabled);
    return makeResult(true, false, mode == FormatMode::FULL ? "FULL OK" : "FORMAT OK");
#endif

    // Fallback path when FATFS not available
    if (allowFallback && Config::isSDAvailable() && wipePorkchopLayout()) {
        reportProgress(cb, "WIPE", 100);
        SDLog::setEnabled(logWasEnabled);
        return makeResult(true, true, "WIPE OK");
    }

    SDLog::setEnabled(logWasEnabled);
    return makeResult(false, allowFallback, "FORMAT FAIL");
}

} // namespace SDFormat
