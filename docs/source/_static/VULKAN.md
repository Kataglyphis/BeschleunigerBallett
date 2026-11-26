Debug Vulkan Code
```powershell
$env:VK_INSTANCE_LAYERS='VK_LAYER_LUNARG_crash_diagnostic'; .\build\BeschleunigerBallett.exe --gpu=integrated *> .\logs\GraphicsEngineVulkan\integrated_crashdiag_latest.log; Remove-Item Env:VK_INSTANCE_LAYERS
```