#pragma once

#include "CoreMinimal.h"

// 前置声明，不提前暴露 OpenSSL
struct AES_KEY_FIXED;
struct LICENSE_INFO_DATA;

struct FLicenseInfo
{
	TCHAR Username[64];
	TCHAR ExpireDate[20];
	int32 FuncLevel;
	bool bPermanent;
};

class LICENSERUNTIME_API FLicenseTool
{
public:
	static bool VerifyLicense(const FString& LicPath, FLicenseInfo& OutInfo);
	static void CreateLicense(const FString& User, const FString& Expire, int32 Level, bool Permanent, const FString& SavePath);
};