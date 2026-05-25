#pragma once
#include "Kismet/BlueprintFunctionLibrary.h"
#include "LicenseBPLibrary.generated.h"

UCLASS()
class LICENSERUNTIME_API ULicenseBPLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "License")
	static bool CheckLicenseValid();
	
	UFUNCTION(BlueprintCallable, Category = "License")
	static bool CreateLicense(const FString& User, const FString& Expire, int32 Level, bool Permanent, const FString& SavePath);
	
};