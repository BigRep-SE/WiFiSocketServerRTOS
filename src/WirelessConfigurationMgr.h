
/*
 * WirelessConfigurationMgr.h
 *
 * Manages the storage of Wi-Fi station/AP configuration and credentials.
 */

#ifndef SRC_WIFI_CONFIGURATION_MANAGER_H_
#define SRC_WIFI_CONFIGURATION_MANAGER_H_

#include <string>
#include <stdint.h>
#include <stdlib.h>

#include "include/MessageFormats.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "flashdb.h"

class WirelessConfigurationMgr
{
public:
	static constexpr int AP = 0;

	static WirelessConfigurationMgr* GetInstance()
	{
		if (!instance)
		{
			instance = new WirelessConfigurationMgr();
		}
		return instance;
	}

	void Init();
	void Reset();

	int SetSsid(const WirelessConfigurationData& data, bool ap);
	bool EraseSsid(const char *ssid);
	bool GetSsid(int ssid, WirelessConfigurationData& data);
	int GetSsid(const char* ssid, WirelessConfigurationData& data);

	bool BeginEnterpriseSsid(const WirelessConfigurationData &data);
	bool SetEnterpriseCredential(int cred, const void* buff, size_t size);
	bool EndEnterpriseSsid(bool cancel);
	const uint8_t* GetEnterpriseCredentials(int ssid, CredentialsInfo& sizes, CredentialsInfo& offsets);

private:
	static WirelessConfigurationMgr* instance;

	static constexpr char KVS_NAME[] = "kvs";
	static constexpr char SSIDS_NS[] = "ssids";

	static constexpr char SCRATCH_NS[] = "scratch";
	static constexpr char CREDS_NS[] = "creds";

	static constexpr char SCRATCH_OFFSET_ID[] = "offset";
	static constexpr char LOADED_SSID_ID[] = "ssid";

	static constexpr int  CREDS_SIZES_IDX = UINT8_MAX;
	struct PendingEnterpriseSsid
	{
		int ssid;
		WirelessConfigurationData data;
		CredentialsInfo sizes;
	};

	const uint8_t* scratchBase;
	PendingEnterpriseSsid* pendingSsid;

	struct fdb_kvdb kvs;
	static SemaphoreHandle_t kvsLock;

	void InitKVS();
	static void LockKVS(fdb_db_t db);
	static void UnlockKVS(fdb_db_t db);
	bool DeleteKV(std::string key);
	bool SetKV(std::string key, const void *buff, size_t sz);
	bool GetKV(std::string key, void* buff, size_t sz);

	std::string GetSsidKey(int ssid);
	bool SetSsidData(int ssid, const WirelessConfigurationData& data);
	bool EraseSsidData(int ssid);
	bool EraseSsid(int ssid);

	const esp_partition_t* GetScratchPartition();
	std::string GetScratchKey(const char* name);
	bool EraseScratch();

	std::string GetCredentialKey(int ssid, int cred, int chunk);
	bool SetCredential(int ssid, int cred, int chunk, const void* buff, size_t sz);
	size_t GetCredential(int ssid, int cred, int chunk, void* buff, size_t sz);
	bool GetCredentialSizes(int ssid, CredentialsInfo& sizes);
	bool EraseCredentials(int ssid);
	bool ResetIfCredentialsLoaded(int ssid);

	int FindEmptySsidEntry();
	bool IsSsidBlank(const WirelessConfigurationData& data);
};

#endif /* SRC_WIFI_CONFIGURATION_MANAGER_H_ */