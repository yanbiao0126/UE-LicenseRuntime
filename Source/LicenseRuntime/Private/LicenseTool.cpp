#include "LicenseTool.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/PlatformProcess.h"
#include "CoreMinimal.h"
#include "Containers/StringConv.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Guid.h"

THIRD_PARTY_INCLUDES_START
// UE 在 ObjectMacros 中定义了 namespace UI，和 OpenSSL 的 UI 类型重名。
// 通过临时宏重命名 OpenSSL 的 UI 标识符，避免 C++ 名字冲突。
#define UI OPENSSL_UI
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/aes.h>
#include <openssl/sha.h>
#undef UI
THIRD_PARTY_INCLUDES_END

struct FLicensePayloadV2Legacy
{
    uint32 Magic;
    uint32 Version;
    ANSICHAR Username[64];
    ANSICHAR ExpireDate[20];
    int32 FuncLevel;
    uint8 bPermanent;
    uint8 ReservedA[7];
    int64 ExpireUnixUtc;
    uint8 ReservedB[16];
};

struct FLicensePayloadV3
{
    uint32 Magic;
    uint32 Version;
    ANSICHAR Username[64];
    ANSICHAR ExpireDate[20];
    int32 FuncLevel;
    uint8 bPermanent;
    uint8 ReservedA[7];
    int64 ExpireUnixUtc;
    ANSICHAR ProjectId[40];
    uint8 ReservedB[8];
};

struct FLicenseStateCore
{
    uint32 Magic;
    uint32 Version;
    int64 LastTrustedUtc;
    double LastMonotonicSeconds;
    uint32 AnomalyCount;
    uint8 LicenseHash[32];
    uint64 Salt;
};

struct FLicenseStateFile
{
    FLicenseStateCore Core;
    uint8 Digest[32];
};

static_assert(sizeof(FLicensePayloadV2Legacy) == 128, "Unexpected legacy payload size.");
static_assert(sizeof(FLicensePayloadV3) % 16 == 0, "License payload must be 16-byte aligned for AES ECB blocks.");

// ==============================================
// 密钥配置
// ==============================================
#define RSA_PUB_KEY \
"-----BEGIN PUBLIC KEY-----\n" \
"MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAs7TMPJmRALyOEHsfYzgv\n" \
"KMP2chgKYxGW+S2ThYtSFBmVwQoRhnI6ir1SQeJo5mt/jtM5rRUR+SbSF68e7f77\n" \
"dAJHhdGYQc4HhyfDiOyMBg+/Inw/10fUGfq4iXadA9HJp1HoZipbrZ3MT6hQCsfD\n" \
"E22VsRZiRJ8NM0ailxZDguUpo9OorbKIi6TZr3NVfLdLBWpEG3JAG+LlB+UCakh4\n" \
"LYbzltTDn5PiMBJwDFHpRCDOYGO7dvhWu6jL/psyHx7PywtPQJPLfwDUzyVBEu99\n" \
"J5d2kDNXdqZcuGhuq4iz/bBjk6p7+hDV8qJGJOV9Wj8w/vp5MTrLAlei4NgNqGW/\n" \
"jwIDAQAB\n" \
"-----END PUBLIC KEY-----"

const uint8 AES_KEY_DATA[16] = {
    0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
    0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00
};

const uint8 STATE_SECRET[32] = {
    0x7A, 0x2D, 0x91, 0x4F, 0x35, 0xBE, 0x18, 0x6C,
    0xC3, 0x54, 0x0B, 0xE8, 0x11, 0x97, 0x62, 0xAD,
    0x40, 0xF2, 0x29, 0x73, 0xD6, 0x8A, 0x1E, 0xB0,
    0x57, 0xC9, 0x24, 0x3D, 0x8F, 0x65, 0xAA, 0x10
};

constexpr uint32 LICENSE_PAYLOAD_MAGIC = 0x3243494C; // "LIC2"
constexpr uint32 LICENSE_PAYLOAD_VERSION = 3;
constexpr uint32 LICENSE_STATE_MAGIC = 0x54534C52; // "RLST"
constexpr uint32 LICENSE_STATE_VERSION = 2;
constexpr const TCHAR* STATE_FILE_RELATIVE_PATH = TEXT("LicenseRuntime/license_state.dat");
constexpr const TCHAR* PROJECT_ID_SECTION = TEXT("/Script/LicenseRuntime.LicenseRuntimeSettings");
constexpr const TCHAR* PROJECT_ID_KEY = TEXT("ProjectId");
constexpr const TCHAR* PROJECT_SETTINGS_SECTION = TEXT("/Script/EngineSettings.GeneralProjectSettings");
constexpr const TCHAR* PROJECT_SETTINGS_ID_KEY = TEXT("ProjectID");

// 允许系统时间回拨的容差（秒）。
// 建议 120~600，过小容易误伤（时钟同步抖动），过大则降低防护强度。
constexpr int64 ALLOW_BACK_SECONDS = 300;

// 墙钟时间与单调时钟允许的最大漂移（秒）。
// 建议 300~1800，用于识别异常时间跳变。
constexpr int64 DRIFT_TOLERANCE_SECONDS = 900;

// 文件系统时间戳允许的误差（秒），用于覆盖文件系统精度和时钟同步抖动。
constexpr int64 FILE_TIME_TOLERANCE_SECONDS = 300;

// 连续检测到时间异常的最大允许次数。
// 达到该阈值后 VerifyLicense 返回失败。
constexpr uint32 MAX_ANOMALY_COUNT = 15;

// ==============================================
// 工具函数
// ==============================================
static void SHA256(const uint8* Data, int Len, uint8 Out[32])
{
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, Data, Len);
    SHA256_Final(Out, &ctx);
}

static bool RSA_Verify(const uint8* Data, int Len, const uint8 Sig[256])
{
    BIO* bp = BIO_new_mem_buf(RSA_PUB_KEY, -1);
    RSA* rsa = PEM_read_bio_RSA_PUBKEY(bp, 0, 0, 0);
    if (!rsa) { BIO_free(bp); return false; }

    uint8 hash[32];
    SHA256(Data, Len, hash);
    int ret = RSA_verify(NID_sha256, hash, 32, (uint8*)Sig, 256, rsa);

    RSA_free(rsa);
    BIO_free(bp);
    return ret == 1;
}

static void AddUniqueStatePath(TArray<FString>& Paths, const FString& Path)
{
    if (Path.IsEmpty())
    {
        return;
    }

    const FString StandardPath = FPaths::ConvertRelativePathToFull(Path);
    if (!Paths.Contains(StandardPath))
    {
        Paths.Add(StandardPath);
    }
}

static TArray<FString> GetStateFilePaths()
{
    TArray<FString> Paths;

    AddUniqueStatePath(Paths, FPaths::ProjectSavedDir() / STATE_FILE_RELATIVE_PATH);
    AddUniqueStatePath(Paths, FPaths::ProjectPersistentDownloadDir() / STATE_FILE_RELATIVE_PATH);
    AddUniqueStatePath(Paths, FString(FPlatformProcess::UserSettingsDir()) / FApp::GetProjectName() / STATE_FILE_RELATIVE_PATH);

    return Paths;
}

static int64 GetNowUtcSeconds()
{
    return FDateTime::UtcNow().ToUnixTimestamp();
}

static bool TryGetFileModifiedUtcSeconds(const FString& FilePath, int64& OutModifiedUtc)
{
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.FileExists(*FilePath))
    {
        return false;
    }

    const FDateTime Timestamp = PlatformFile.GetTimeStamp(*FilePath);
    if (Timestamp == FDateTime::MinValue())
    {
        return false;
    }

    OutModifiedUtc = Timestamp.ToUnixTimestamp();
    return true;
}

static bool NormalizeProjectId(const FString& InProjectId, FString& OutNormalizedProjectId)
{
    const FString TrimmedProjectId = InProjectId.TrimStartAndEnd();
    if (TrimmedProjectId.IsEmpty())
    {
        return false;
    }

    FGuid ParsedGuid;
    const bool bParsed =
        FGuid::Parse(TrimmedProjectId, ParsedGuid) ||
        FGuid::ParseExact(TrimmedProjectId, EGuidFormats::DigitsWithHyphens, ParsedGuid) ||
        FGuid::ParseExact(TrimmedProjectId, EGuidFormats::DigitsWithHyphensInBraces, ParsedGuid) ||
        FGuid::ParseExact(TrimmedProjectId, EGuidFormats::DigitsWithHyphensInParentheses, ParsedGuid) ||
        FGuid::ParseExact(TrimmedProjectId, EGuidFormats::Digits, ParsedGuid);
    if (!bParsed)
    {
        return false;
    }

    OutNormalizedProjectId = ParsedGuid.ToString(EGuidFormats::DigitsWithHyphens).ToUpper();
    return true;
}

static bool GetConfiguredProjectId(FString& OutProjectId)
{
    if (!GConfig)
    {
        UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: 配置系统未就绪，无法读取项目ID。"));
        return false;
    }

    FString RawProjectId;
    if (GConfig->GetString(PROJECT_ID_SECTION, PROJECT_ID_KEY, RawProjectId, GGameIni))
    {
        if (NormalizeProjectId(RawProjectId, OutProjectId))
        {
            return true;
        }

        UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: 项目ID格式非法（需GUID），当前值: %s"), *RawProjectId);
        return false;
    }

    if (GConfig->GetString(PROJECT_SETTINGS_SECTION, PROJECT_SETTINGS_ID_KEY, RawProjectId, GGameIni))
    {
        if (NormalizeProjectId(RawProjectId, OutProjectId))
        {
            UE_LOG(LogTemp, Log, TEXT("LicenseRuntime: 使用 GeneralProjectSettings.ProjectID 作为授权项目ID。"));
            return true;
        }

        UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: GeneralProjectSettings.ProjectID 格式非法（需GUID），当前值: %s"), *RawProjectId);
        return false;
    }

    UE_LOG(
        LogTemp,
        Error,
        TEXT("LicenseRuntime: 缺少项目ID配置。请配置 [%s] %s，或设置 [%s] %s。"),
        PROJECT_ID_SECTION,
        PROJECT_ID_KEY,
        PROJECT_SETTINGS_SECTION,
        PROJECT_SETTINGS_ID_KEY);
    return false;
}
static bool ParseExpireDateToUtc(const FString& ExpireDate, int64& OutUnixUtc)
{
    TArray<FString> Parts;
    ExpireDate.ParseIntoArray(Parts, TEXT("-"), true);
    if (Parts.Num() != 3)
    {
        return false;
    }

    const int32 Year = FCString::Atoi(*Parts[0]);
    const int32 Month = FCString::Atoi(*Parts[1]);
    const int32 Day = FCString::Atoi(*Parts[2]);
    if (Year <= 0 || Month <= 0 || Day <= 0)
    {
        return false;
    }

    FDateTime ExpireDateTime;
    if (!FDateTime::Validate(Year, Month, Day, 23, 59, 59, 0))
    {
        return false;
    }

    ExpireDateTime = FDateTime(Year, Month, Day, 23, 59, 59);
    OutUnixUtc = ExpireDateTime.ToUnixTimestamp();
    return true;
}

static void BuildStateDigest(const FLicenseStateCore& Core, uint8 OutDigest[32])
{
    uint8 Buffer[sizeof(STATE_SECRET) + sizeof(FLicenseStateCore)];
    FMemory::Memcpy(Buffer, STATE_SECRET, sizeof(STATE_SECRET));
    FMemory::Memcpy(Buffer + sizeof(STATE_SECRET), &Core, sizeof(FLicenseStateCore));
    SHA256(Buffer, sizeof(Buffer), OutDigest);
}

static bool SaveStateToPath(const FLicenseStateCore& Core, const FString& StatePath)
{
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    PlatformFile.CreateDirectoryTree(*FPaths::GetPath(StatePath));

    FLicenseStateFile StateFile{};
    StateFile.Core = Core;
    BuildStateDigest(StateFile.Core, StateFile.Digest);

    IFileHandle* Handle = PlatformFile.OpenWrite(*StatePath);
    if (!Handle)
    {
        UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: 无法写入状态文件: %s"), *StatePath);
        return false;
    }

    const bool bOk = Handle->Write(reinterpret_cast<const uint8*>(&StateFile), sizeof(StateFile));
    delete Handle;
    return bOk;
}

static bool LoadStateFromPath(const FString& StatePath, FLicenseStateCore& OutCore)
{
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.FileExists(*StatePath))
    {
        return false;
    }

    IFileHandle* Handle = PlatformFile.OpenRead(*StatePath);
    if (!Handle)
    {
        UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: 无法读取状态文件: %s"), *StatePath);
        return false;
    }

    const int64 FileSize = Handle->Size();
    if (FileSize != sizeof(FLicenseStateFile))
    {
        delete Handle;
        UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: 状态文件尺寸异常，疑似篡改: %s"), *StatePath);
        return false;
    }

    FLicenseStateFile StateFile{};
    const bool bReadOk = Handle->Read(reinterpret_cast<uint8*>(&StateFile), sizeof(StateFile));
    delete Handle;
    if (!bReadOk)
    {
        return false;
    }

    if (StateFile.Core.Magic != LICENSE_STATE_MAGIC || StateFile.Core.Version != LICENSE_STATE_VERSION)
    {
        UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: 状态文件版本异常，疑似篡改: %s"), *StatePath);
        return false;
    }

    uint8 ExpectedDigest[32];
    BuildStateDigest(StateFile.Core, ExpectedDigest);
    if (FMemory::Memcmp(ExpectedDigest, StateFile.Digest, 32) != 0)
    {
        UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: 状态文件校验失败，疑似篡改: %s"), *StatePath);
        return false;
    }

    OutCore = StateFile.Core;
    return true;
}

static bool SaveStateToAllPaths(const FLicenseStateCore& Core)
{
    const TArray<FString> StatePaths = GetStateFilePaths();
    bool bAllSaved = true;
    for (const FString& StatePath : StatePaths)
    {
        if (!SaveStateToPath(Core, StatePath))
        {
            bAllSaved = false;
        }
    }
    return bAllSaved;
}

static void InitializeStateCore(FLicenseStateCore& OutCore, int64 NowUtc, double NowMono, const uint8 CurrentLicenseHash[32])
{
    OutCore = {};
    OutCore.Magic = LICENSE_STATE_MAGIC;
    OutCore.Version = LICENSE_STATE_VERSION;
    OutCore.LastTrustedUtc = NowUtc;
    OutCore.LastMonotonicSeconds = NowMono;
    OutCore.AnomalyCount = 0;
    FMemory::Memcpy(OutCore.LicenseHash, CurrentLicenseHash, 32);
    OutCore.Salt = static_cast<uint64>(FPlatformTime::Cycles64()) ^ static_cast<uint64>(NowUtc);
}

static bool VerifyOfflineTimeAndUpdateState(const uint8 CurrentLicenseHash[32], const FString& LicensePath)
{
    const int64 NowUtc = GetNowUtcSeconds();
    const double NowMono = FPlatformTime::Seconds();
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    const TArray<FString> StatePaths = GetStateFilePaths();

    TArray<FLicenseStateCore> ValidStates;
    int32 ExistingFileCount = 0;
    int32 InvalidFileCount = 0;
    int32 TimestampAnomalyCount = 0;

    int64 LicenseModifiedUtc = 0;
    if (TryGetFileModifiedUtcSeconds(LicensePath, LicenseModifiedUtc) &&
        LicenseModifiedUtc > NowUtc + FILE_TIME_TOLERANCE_SECONDS)
    {
        ++TimestampAnomalyCount;
        UE_LOG(
            LogTemp,
            Error,
            TEXT("LicenseRuntime: License 文件修改时间晚于当前系统时间，疑似系统时间回拨或文件时间篡改: %s"),
            *LicensePath);
    }

    for (const FString& StatePath : StatePaths)
    {
        if (!PlatformFile.FileExists(*StatePath))
        {
            continue;
        }

        ++ExistingFileCount;
        FLicenseStateCore State{};
        if (LoadStateFromPath(StatePath, State))
        {
            int64 StateModifiedUtc = 0;
            if (TryGetFileModifiedUtcSeconds(StatePath, StateModifiedUtc))
            {
                const bool bModifiedInFuture = StateModifiedUtc > NowUtc + FILE_TIME_TOLERANCE_SECONDS;
                const bool bModifiedBeforeTrusted = StateModifiedUtc + FILE_TIME_TOLERANCE_SECONDS < State.LastTrustedUtc;
                if (bModifiedInFuture || bModifiedBeforeTrusted)
                {
                    ++TimestampAnomalyCount;
                    UE_LOG(
                        LogTemp,
                        Error,
                        TEXT("LicenseRuntime: 状态文件修改时间异常，疑似系统时间回拨或文件时间篡改: %s"),
                        *StatePath);
                }
            }
            ValidStates.Add(State);
        }
        else
        {
            ++InvalidFileCount;
        }
    }

    if (TimestampAnomalyCount > 0)
    {
        UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: 检测到文件时间戳异常，拒绝授权校验。"));
        return false;
    }

    if (ExistingFileCount == 0)
    {
        FLicenseStateCore Initial{};
        InitializeStateCore(Initial, NowUtc, NowMono, CurrentLicenseHash);
        return SaveStateToAllPaths(Initial);
    }

    if (ValidStates.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: 已存在状态文件但全部不可用，拒绝自动重建。"));
        return false;
    }

    FLicenseStateCore Previous = ValidStates[0];
    for (int32 Index = 1; Index < ValidStates.Num(); ++Index)
    {
        const FLicenseStateCore& Candidate = ValidStates[Index];
        if (Candidate.LastTrustedUtc > Previous.LastTrustedUtc)
        {
            Previous = Candidate;
        }
    }

    const int64 DeltaUtc = NowUtc - Previous.LastTrustedUtc;
    const double DeltaMonoFloat = NowMono - Previous.LastMonotonicSeconds;
    const int64 DeltaMono = static_cast<int64>(DeltaMonoFloat < 0.0 ? 0.0 : DeltaMonoFloat);
    const int64 Drift = FMath::Abs(DeltaUtc - DeltaMono);

    const bool bRollback = DeltaUtc < -ALLOW_BACK_SECONDS;
    const bool bDriftAnomaly = Drift > DRIFT_TOLERANCE_SECONDS;
    const bool bMissingReplica = ExistingFileCount < StatePaths.Num();
    const bool bCorruptReplica = InvalidFileCount > 0;
    const bool bTimestampAnomaly = TimestampAnomalyCount > 0;
    const bool bHardTimeAnomaly = bRollback || bTimestampAnomaly;
    const bool bAnomaly = bHardTimeAnomaly || bMissingReplica || bCorruptReplica || bTimestampAnomaly;

    FLicenseStateCore Next = Previous;
    Next.LastMonotonicSeconds = NowMono;
    if (NowUtc > Next.LastTrustedUtc)
    {
        Next.LastTrustedUtc = NowUtc;
    }
    FMemory::Memcpy(Next.LicenseHash, CurrentLicenseHash, 32);

    if (bAnomaly)
    {
        Next.AnomalyCount = Previous.AnomalyCount + 1;
        SaveStateToAllPaths(Next);

        if (bHardTimeAnomaly)
        {
            UE_LOG(
                LogTemp,
                Error,
                TEXT("LicenseRuntime: 检测到严重授权时间异常，DeltaUtc=%lld, DeltaMono=%lld, Drift=%lld, MissingReplica=%d, CorruptReplica=%d, TimestampAnomaly=%d, Count=%u"),
                DeltaUtc,
                DeltaMono,
                Drift,
                bMissingReplica ? 1 : 0,
                bCorruptReplica ? 1 : 0,
                bTimestampAnomaly ? 1 : 0,
                Next.AnomalyCount
            );
            return false;
        }

        UE_LOG(
            LogTemp,
            Warning,
            TEXT("LicenseRuntime: 检测到授权状态异常，DeltaUtc=%lld, DeltaMono=%lld, Drift=%lld, MissingReplica=%d, CorruptReplica=%d, TimestampAnomaly=%d, Count=%u"),
            DeltaUtc,
            DeltaMono,
            Drift,
            bMissingReplica ? 1 : 0,
            bCorruptReplica ? 1 : 0,
            bTimestampAnomaly ? 1 : 0,
            Next.AnomalyCount
        );
        return Next.AnomalyCount < MAX_ANOMALY_COUNT;
    }

    Next.AnomalyCount = 0;
    return SaveStateToAllPaths(Next);
}

static bool LoadPrivateKeyPem(FString& OutPrivateKeyPem)
{
    const FString PrivateKeyPath = FPaths::ProjectDir() / TEXT("private.key");
    if (!FFileHelper::LoadFileToString(OutPrivateKeyPem, *PrivateKeyPath))
    {
        UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: 无法读取私钥文件: %s"), *PrivateKeyPath);
        return false;
    }

    return true;
}

static bool RSA_Sign(const uint8* Data, int Len, uint8 OutSig[256])
{
    FString PrivateKeyPem;
    if (!LoadPrivateKeyPem(PrivateKeyPem))
    {
        return false;
    }

    FTCHARToUTF8 PrivateKeyUtf8(*PrivateKeyPem);
    BIO* bp = BIO_new_mem_buf((void*)PrivateKeyUtf8.Get(), PrivateKeyUtf8.Length());
    if (!bp)
    {
        return false;
    }

    RSA* rsa = PEM_read_bio_RSAPrivateKey(bp, 0, 0, 0);
    if (!rsa) { BIO_free(bp); return false; }

    uint8 hash[32];
    SHA256(Data, Len, hash);
    uint32 slen = 0;
    const int signRet = RSA_sign(NID_sha256, hash, 32, OutSig, &slen, rsa);

    RSA_free(rsa);
    BIO_free(bp);
    return signRet == 1 && slen == 256;
}

static void AES_DecryptFixed(const uint8* In, uint8* Out, int Len)
{
    AES_KEY aesKey;
    AES_set_decrypt_key(AES_KEY_DATA, 128, &aesKey);
    for (int i = 0; i < Len; i += 16)
        AES_ecb_encrypt(In + i, Out + i, &aesKey, AES_DECRYPT);
}

static void AES_EncryptFixed(const uint8* In, uint8* Out, int Len)
{
    AES_KEY aesKey;
    AES_set_encrypt_key(AES_KEY_DATA, 128, &aesKey);
    for (int i = 0; i < Len; i += 16)
        AES_ecb_encrypt(In + i, Out + i, &aesKey, AES_ENCRYPT);
}

static bool VerifyLicensePayloadV3(
    const uint8* EncryptedPayload,
    const uint8 Sig[256],
    const uint8 CurrentLicenseHash[32],
    const FString& LicensePath,
    const FString& ConfiguredProjectId,
    FLicenseInfo& OutInfo)
{
    if (!RSA_Verify(EncryptedPayload, sizeof(FLicensePayloadV3), Sig))
    {
        return false;
    }

    FLicensePayloadV3 Payload{};
    AES_DecryptFixed(EncryptedPayload, reinterpret_cast<uint8*>(&Payload), sizeof(Payload));

    if (Payload.Magic != LICENSE_PAYLOAD_MAGIC || Payload.Version != LICENSE_PAYLOAD_VERSION)
    {
        return false;
    }

    FString NormalizedPayloadProjectId;
    if (!NormalizeProjectId(UTF8_TO_TCHAR(Payload.ProjectId), NormalizedPayloadProjectId))
    {
        UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: License 中的项目ID格式非法。"));
        return false;
    }

    if (!NormalizedPayloadProjectId.Equals(ConfiguredProjectId, ESearchCase::CaseSensitive))
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("LicenseRuntime: 项目ID不匹配，拒绝复用授权。License=%s, Project=%s"),
            *NormalizedPayloadProjectId,
            *ConfiguredProjectId);
        return false;
    }

    const int64 NowUtc = GetNowUtcSeconds();
    if (Payload.bPermanent == 0 && NowUtc > Payload.ExpireUnixUtc)
    {
        return false;
    }

    if (!VerifyOfflineTimeAndUpdateState(CurrentLicenseHash, LicensePath))
    {
        return false;
    }

    FCString::Strncpy(OutInfo.Username, UTF8_TO_TCHAR(Payload.Username), UE_ARRAY_COUNT(OutInfo.Username));
    FCString::Strncpy(OutInfo.ExpireDate, UTF8_TO_TCHAR(Payload.ExpireDate), UE_ARRAY_COUNT(OutInfo.ExpireDate));
    OutInfo.FuncLevel = Payload.FuncLevel;
    OutInfo.bPermanent = (Payload.bPermanent != 0);
    OutInfo.ExpireUnixUtc = Payload.ExpireUnixUtc;
    return true;
}

// ==============================================
// 校验 License
// ==============================================
bool FLicenseTool::VerifyLicense(const FString& LicPath, FLicenseInfo& OutInfo)
{
    FString ConfiguredProjectId;
    if (!GetConfiguredProjectId(ConfiguredProjectId))
    {
        return false;
    }

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    IFileHandle* Handle = PlatformFile.OpenRead(*LicPath);
    if (!Handle)
    {
        return false;
    }

    const int64 FileSize = Handle->Size();
    uint8 Sig[256];

    if (FileSize == static_cast<int64>(sizeof(FLicensePayloadV3) + sizeof(Sig)))
    {
        uint8 EncryptedPayload[sizeof(FLicensePayloadV3)];
        const bool bReadPayload = Handle->Read(EncryptedPayload, sizeof(EncryptedPayload));
        const bool bReadSig = Handle->Read(Sig, sizeof(Sig));
        delete Handle;
        if (!bReadPayload || !bReadSig)
        {
            return false;
        }
        uint8 CurrentLicenseHash[32];
        uint8 RawLicenseBytes[sizeof(FLicensePayloadV3) + sizeof(Sig)];
        FMemory::Memcpy(RawLicenseBytes, EncryptedPayload, sizeof(EncryptedPayload));
        FMemory::Memcpy(RawLicenseBytes + sizeof(EncryptedPayload), Sig, sizeof(Sig));
        SHA256(RawLicenseBytes, sizeof(RawLicenseBytes), CurrentLicenseHash);
        return VerifyLicensePayloadV3(EncryptedPayload, Sig, CurrentLicenseHash, LicPath, ConfiguredProjectId, OutInfo);
    }

    if (FileSize == static_cast<int64>(sizeof(FLicensePayloadV2Legacy) + sizeof(Sig)))
    {
        delete Handle;
        UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: 检测到旧版V2授权文件，已强制失效，请重新签发V3 License: %s"), *LicPath);
        return false;
    }

    delete Handle;
    UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: License 文件尺寸不匹配或版本过旧，请重新生成授权文件: %s"), *LicPath);
    return false;
}

// ==============================================
// 创建 License
// ==============================================
bool FLicenseTool::CreateLicense(const FString& User, const FString& Expire, int32 Level, bool Permanent, const FString& SavePath)
{
#if !WITH_EDITOR
    UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: CreateLicense 仅支持编辑器环境。"));
    return false;
#else
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    FString ConfiguredProjectId;
    if (!GetConfiguredProjectId(ConfiguredProjectId))
    {
        return false;
    }

    int64 ExpireUnixUtc = 0;
    if (!Permanent && !ParseExpireDateToUtc(Expire, ExpireUnixUtc))
    {
        UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: Expire 参数格式错误，要求 YYYY-MM-DD: %s"), *Expire);
        return false;
    }

    FLicensePayloadV3 Payload{};
    Payload.Magic = LICENSE_PAYLOAD_MAGIC;
    Payload.Version = LICENSE_PAYLOAD_VERSION;
    FCStringAnsi::Strncpy(Payload.Username, TCHAR_TO_UTF8(*User), UE_ARRAY_COUNT(Payload.Username));
    FCStringAnsi::Strncpy(Payload.ExpireDate, TCHAR_TO_UTF8(*Expire), UE_ARRAY_COUNT(Payload.ExpireDate));
    FCStringAnsi::Strncpy(Payload.ProjectId, TCHAR_TO_UTF8(*ConfiguredProjectId), UE_ARRAY_COUNT(Payload.ProjectId));
    Payload.FuncLevel = Level;
    Payload.bPermanent = Permanent ? 1 : 0;
    Payload.ExpireUnixUtc = Permanent ? TNumericLimits<int64>::Max() : ExpireUnixUtc;

    FLicensePayloadV3 EncryptedPayload{};
    AES_EncryptFixed(reinterpret_cast<const uint8*>(&Payload), reinterpret_cast<uint8*>(&EncryptedPayload), sizeof(EncryptedPayload));

    uint8 Sig[256];
    if (!RSA_Sign(reinterpret_cast<const uint8*>(&EncryptedPayload), sizeof(EncryptedPayload), Sig))
    {
        UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: License 签名失败，未写入文件: %s"), *SavePath);
        return false;
    }

    IFileHandle* Handle = PlatformFile.OpenWrite(*SavePath);
    if (Handle)
    {
        const bool bWritePayloadOk = Handle->Write(reinterpret_cast<const uint8*>(&EncryptedPayload), sizeof(EncryptedPayload));
        const bool bWriteSigOk = Handle->Write(Sig, 256);
        delete Handle;
        if (!bWritePayloadOk || !bWriteSigOk)
        {
            UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: License 文件写入不完整: %s"), *SavePath);
            return false;
        }
        return true;
    }
    
    UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: 无法写入 License 文件: %s"), *SavePath);
    return false;
#endif
}
