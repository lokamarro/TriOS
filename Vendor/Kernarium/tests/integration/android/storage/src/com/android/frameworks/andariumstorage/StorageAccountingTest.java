// SPDX-License-Identifier: Apache-2.0
package com.android.frameworks.andariumstorage;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import android.app.usage.StorageStatsManager;
import android.content.Context;
import android.os.Bundle;
import android.os.StatFs;
import android.os.storage.StorageManager;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class StorageAccountingTest {
    @Test
    public void exactFilesystemBytes() throws Exception {
        long totalBytes = 0;
        long osBytes = 0;
        for (String path : new String[] {
                "/data", "/system", "/system_ext", "/product", "/vendor", "/odm"}) {
            final long bytes = new StatFs(path).getTotalBytes();
            totalBytes += bytes;
            if (!path.equals("/data")) {
                osBytes += bytes;
            }
        }
        final Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        final StorageManager storage = context.getSystemService(StorageManager.class);
        assertEquals(totalBytes, storage.getPrimaryStorageSize());
        assertEquals(totalBytes,
                StorageManager.getPrimaryStoragePathAndSize().second.longValue());
        final StorageStatsManager stats = context.getSystemService(StorageStatsManager.class);
        assertEquals(totalBytes, stats.getTotalBytes(StorageManager.UUID_DEFAULT));
        final long freeBytes = stats.getFreeBytes(StorageManager.UUID_DEFAULT);
        assertTrue(freeBytes >= 0);
        assertTrue(freeBytes <= new StatFs("/data").getTotalBytes());
        final Bundle result = new Bundle();
        result.putLong("storage_total_bytes", totalBytes);
        result.putLong("storage_os_bytes", osBytes);
        result.putLong("storage_free_bytes", freeBytes);
        InstrumentationRegistry.getInstrumentation().sendStatus(2, result);
    }
}
