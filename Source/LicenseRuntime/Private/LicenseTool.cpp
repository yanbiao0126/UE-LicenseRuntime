#include "LicenseTool.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "CoreMinimal.h"
#include "Containers/StringConv.h"

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

// ==============================================
// 校验 License
// ==============================================
bool FLicenseTool::VerifyLicense(const FString& LicPath, FLicenseInfo& OutInfo)
{
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    IFileHandle* Handle = PlatformFile.OpenRead(*LicPath);
    if (!Handle) return false;

    FLicenseInfo Info;
    uint8 Sig[256];
    Handle->Read((uint8*)&Info, sizeof(Info));
    Handle->Read(Sig, 256);
    delete Handle;

    if (!RSA_Verify((uint8*)&Info, sizeof(Info), Sig))
        return false;

    FLicenseInfo DecryptedInfo;
    AES_DecryptFixed((uint8*)&Info, (uint8*)&DecryptedInfo, sizeof(DecryptedInfo));

    FString NowDate = FDateTime::Now().ToString(TEXT("%Y-%m-%d"));
    if (!DecryptedInfo.bPermanent && NowDate > DecryptedInfo.ExpireDate)
        return false;

    OutInfo = DecryptedInfo;
    return true;
}

// ==============================================
// 创建 License
// ==============================================
void FLicenseTool::CreateLicense(const FString& User, const FString& Expire, int32 Level, bool Permanent, const FString& SavePath)
{
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

    FLicenseInfo Info{};
    FCString::Strcpy(Info.Username, *User);
    FCString::Strcpy(Info.ExpireDate, *Expire);
    Info.FuncLevel = Level;
    Info.bPermanent = Permanent;

    FLicenseInfo EncryptedInfo;
    AES_EncryptFixed((uint8*)&Info, (uint8*)&EncryptedInfo, sizeof(EncryptedInfo));

    uint8 Sig[256];
    if (!RSA_Sign((uint8*)&EncryptedInfo, sizeof(EncryptedInfo), Sig))
    {
        UE_LOG(LogTemp, Error, TEXT("LicenseRuntime: License 签名失败，未写入文件: %s"), *SavePath);
        return;
    }

    IFileHandle* Handle = PlatformFile.OpenWrite(*SavePath);
    if (Handle)
    {
        Handle->Write((uint8*)&EncryptedInfo, sizeof(EncryptedInfo));
        Handle->Write(Sig, 256);
        delete Handle;
    }
}