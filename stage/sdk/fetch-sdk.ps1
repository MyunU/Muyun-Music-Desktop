# 重新拉取 MuyunStage 构建/运行所需的第三方文件（一次性，产物已 gitignore）
# 用法：在 stage/sdk 目录下 `pwsh -File fetch-sdk.ps1`
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$sdk = $PSScriptRoot
$web = Join-Path (Split-Path $sdk -Parent) 'web'
New-Item -ItemType Directory -Force -Path "$sdk\include", "$web\vendor", "$web\mineradio" | Out-Null

# 1) WebView2 SDK（官方头 + x64 Loader DLL）
Invoke-WebRequest 'https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2' -OutFile "$sdk\webview2.zip"
Expand-Archive "$sdk\webview2.zip" "$sdk\_wv2" -Force
$hdr = Get-ChildItem "$sdk\_wv2" -Recurse -Filter WebView2.h | Select-Object -First 1
$dll = Get-ChildItem "$sdk\_wv2" -Recurse -Filter WebView2Loader.dll | Where-Object FullName -match 'x64' | Select-Object -First 1
Copy-Item $hdr.FullName "$sdk\include\WebView2.h" -Force
Copy-Item $dll.FullName "$sdk\WebView2Loader.dll" -Force
Remove-Item "$sdk\_wv2","$sdk\webview2.zip" -Recurse -Force

# 2) three@0.128.0（MIT）ESM 单文件
Invoke-WebRequest 'https://registry.npmjs.org/three/-/three-0.128.0.tgz' -OutFile "$sdk\three.tgz"
tar -xzf "$sdk\three.tgz" -C "$sdk"
Copy-Item "$sdk\package\build\three.module.js" "$web\vendor\three.module.js" -Force
Remove-Item "$sdk\package","$sdk\three.tgz" -Recurse -Force

# 3) gsap（UMD）
$gsapVer = ((Invoke-RestMethod 'https://registry.npmjs.org/gsap').'dist-tags'.latest)
Invoke-WebRequest "https://registry.npmjs.org/gsap/-/gsap-$gsapVer.tgz" -OutFile "$sdk\gsap.tgz"
tar -xzf "$sdk\gsap.tgz" -C "$sdk"
Copy-Item "$sdk\package\dist\gsap.min.js" "$web\vendor\gsap.min.js" -Force
Remove-Item "$sdk\package","$sdk\gsap.tgz" -Recurse -Force

Write-Host "OK: WebView2 SDK + three + gsap 已就位。" -ForegroundColor Green
Write-Host "music-tempo.min.js 与 mineradio/engine.js+css+资产 从 Sollin 参考工程拷贝（见 HANDOFF 第八节）。" -ForegroundColor Yellow
