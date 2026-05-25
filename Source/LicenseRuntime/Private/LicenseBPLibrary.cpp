#include "LicenseBPLibrary.h"
#include "LicenseTool.h"
#include "Misc/Paths.h"

bool ULicenseBPLibrary::CheckLicenseValid()
{
	FString Path = FPaths::ProjectDir() / TEXT("app.lic");
	FLicenseInfo Info;
	return FLicenseTool::VerifyLicense(Path, Info);
}

bool ULicenseBPLibrary::CreateLicense(const FString& User, const FString& Expire, int32 Level, bool Permanent,
	const FString& SavePath)
{
	FLicenseTool::CreateLicense(User, Expire, Level, Permanent, SavePath);
	return true;
}
