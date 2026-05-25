#pragma once

#include "CoreMinimal.h"

struct FLicenseInfo
{
	TCHAR Username[64];
	TCHAR ExpireDate[20];
	int32 FuncLevel;
	bool bPermanent;
	int64 ExpireUnixUtc;
};

class LICENSERUNTIME_API FLicenseTool
{
public:
	static bool VerifyLicense(const FString& LicPath, FLicenseInfo& OutInfo);
	static bool CreateLicense(const FString& User, const FString& Expire, int32 Level, bool Permanent, const FString& SavePath);
};