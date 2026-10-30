#include "PartitionWriter.h"
#include "esp_log.h"
#include "spi_flash_mmap.h"   // SPI_FLASH_SEC_SIZE (flash erase granularity)

const esp_partition_t* PartitionWriter::Resolve(const char* label, const char** err)
{
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!p) { *err = "unknown partition"; return nullptr; }

    // Never touch the slot we booted from, whatever the caller asked for.
    if (p->type == ESP_PARTITION_TYPE_APP && p == esp_ota_get_running_partition())
    {
        *err = "partition is running";
        return nullptr;
    }
    return p;
}

PartitionWriter::PartitionWriter(const char* label, size_t startOffset,
                                 const char** err)
{
    const esp_partition_t* p = Resolve(label, err);
    if (!p) return;

    if (startOffset >= p->size) { *err = "offset beyond partition"; return; }

    offset_   = startOffset;

    p_ = p;   // ok() now true
}

bool PartitionWriter::write(const void* data, size_t size)
{
    if (!p_) return false;

    const size_t pos = offset_ + written_;
    const size_t end = pos + size;
    if (end > p_->size)
    {
        ESP_LOGE(TAG, "write exceeds partition size");
        return false;
    }

    esp_err_t e = esp_partition_write(p_, pos, data, size);
    if (e != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_partition_write @0x%lx: %s",
                 (unsigned long)pos, esp_err_to_name(e));
        return false;
    }

    written_ += size;
    return true;
}

const char* PartitionWriter::Clear(const char* label)
{
    const char* err = nullptr;
    const esp_partition_t* p = Resolve(label, &err);
    if (!p) return err;

    // Partition sizes are sector multiples, so the whole range is erasable as-is.
    esp_err_t e = esp_partition_erase_range(p, 0, p->size);
    if (e != ESP_OK)
    {
        ESP_LOGE(TAG, "erase '%s': %s", label, esp_err_to_name(e));
        return "erase failed";
    }

    ESP_LOGI(TAG, "cleared '%s' (%lu bytes)", label, (unsigned long)p->size);
    return nullptr;
}

const char* PartitionWriter::Activate(const char* label)
{
    const char* err = nullptr;
    const esp_partition_t* p = Resolve(label, &err);
    if (!p) return err;

    if (p->type != ESP_PARTITION_TYPE_APP)
    {
        // Data partitions have no boot pointer; succeeding keeps the caller's
        // sequence uniform (clear → write → activate) whatever it is uploading.
        ESP_LOGI(TAG, "'%s' is a data partition - nothing to activate", label);
        return nullptr;
    }

    // Validates the image and refuses an invalid one, which is what makes raw
    // offset writes safe: a truncated image never becomes bootable.
    esp_err_t e = esp_ota_set_boot_partition(p);
    if (e != ESP_OK)
    {
        ESP_LOGE(TAG, "set_boot_partition '%s': %s", label, esp_err_to_name(e));
        return "image validation failed";
    }

    ESP_LOGI(TAG, "app image activated, next boot '%s'", label);
    return nullptr;
}
