// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class LicenseRuntime : ModuleRules
{
	public LicenseRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		bEnableExceptions = true;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core", "CoreUObject", "Engine"
				// ... add other public dependencies that you statically link with here ...
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine"
				// ... add private dependencies that you statically link with here ...	
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
		
		// 引入 OpenSSL（UE 内置）
		if (Target.Platform == UnrealTargetPlatform.Win64 ||
		    Target.Platform == UnrealTargetPlatform.Linux)
		{
			AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
		}

		// 打包时自动将 ProjectDir/app.lic 放入打包结果（NonUFS，保持为独立文件）。
		// 运行时 CheckLicenseValid() 仍按 ProjectDir/app.lic 读取即可。
		if (Target.ProjectFile != null)
		{
			string LicensePath = Path.Combine(Target.ProjectFile.Directory.FullName, "app.lic");
			if (File.Exists(LicensePath))
			{
				RuntimeDependencies.Add(LicensePath, StagedFileType.NonUFS);
			}
			else
			{
				System.Console.WriteLine("[LicenseRuntime] app.lic not found, skip staging: " + LicensePath);
			}
		}
		else
		{
			System.Console.WriteLine("[LicenseRuntime] ProjectFile is null, skip app.lic staging.");
		}
	}
}
