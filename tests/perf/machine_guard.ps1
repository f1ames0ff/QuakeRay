if (-not ('QuakeRayMachineGuardProcessTree' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class QuakeRayMachineGuardProcessTree
{
    private const uint TH32CS_SNAPPROCESS = 0x00000002;
    private const uint INVALID_HANDLE_VALUE = 0xFFFFFFFF;
    private const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
    private const int ProcessCommandLineInformation = 60;
    private const int ENUM_CURRENT_SETTINGS = -1;
    private const uint DISPLAY_DEVICE_ATTACHED_TO_DESKTOP = 0x00000001;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct PROCESSENTRY32W
    {
        public uint dwSize;
        public uint cntUsage;
        public uint th32ProcessID;
        public IntPtr th32DefaultHeapID;
        public uint th32ModuleID;
        public uint cntThreads;
        public uint th32ParentProcessID;
        public int pcPriClassBase;
        public uint dwFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        public string szExeFile;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct UNICODE_STRING
    {
        public ushort Length;
        public ushort MaximumLength;
        public IntPtr Buffer;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct DISPLAY_DEVICEW
    {
        public uint cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string DeviceString;
        public uint StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string DeviceKey;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct POINTL
    {
        public int x;
        public int y;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct DEVMODEW
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string dmDeviceName;
        public ushort dmSpecVersion;
        public ushort dmDriverVersion;
        public ushort dmSize;
        public ushort dmDriverExtra;
        public uint dmFields;
        public POINTL dmPosition;
        public uint dmDisplayOrientation;
        public uint dmDisplayFixedOutput;
        public short dmColor;
        public short dmDuplex;
        public short dmYResolution;
        public short dmTTOption;
        public short dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string dmFormName;
        public ushort dmLogPixels;
        public uint dmBitsPerPel;
        public uint dmPelsWidth;
        public uint dmPelsHeight;
        public uint dmDisplayFlags;
        public uint dmDisplayFrequency;
        public uint dmICMMethod;
        public uint dmICMIntent;
        public uint dmMediaType;
        public uint dmDitherType;
        public uint dmReserved1;
        public uint dmReserved2;
        public uint dmPanningWidth;
        public uint dmPanningHeight;
    }

    public sealed class DisplayModeInfo
    {
        public string Name { get; set; }
        public string PnpDeviceId { get; set; }
        public string DriverVersion { get; set; }
        public uint Width { get; set; }
        public uint Height { get; set; }
        public uint RefreshHz { get; set; }
        public string VideoModeDescription { get; set; }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint processId);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool Process32FirstW(IntPtr snapshot, ref PROCESSENTRY32W entry);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool Process32NextW(IntPtr snapshot, ref PROCESSENTRY32W entry);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inheritHandle, uint processId);

    [DllImport("ntdll.dll")]
    private static extern int NtQueryInformationProcess(
        IntPtr process,
        int processInformationClass,
        IntPtr processInformation,
        int processInformationLength,
        out int returnLength);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool EnumDisplayDevicesW(
        string device,
        uint deviceIndex,
        ref DISPLAY_DEVICEW displayDevice,
        uint flags);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool EnumDisplaySettingsW(
        string deviceName,
        int modeIndex,
        ref DEVMODEW deviceMode);

    public static uint GetParentProcessId(uint processId)
    {
        IntPtr snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == IntPtr.Zero || snapshot == new IntPtr(INVALID_HANDLE_VALUE))
            throw new Win32Exception(Marshal.GetLastWin32Error());

        try
        {
            PROCESSENTRY32W entry = new PROCESSENTRY32W();
            entry.dwSize = (uint)Marshal.SizeOf(typeof(PROCESSENTRY32W));
            if (!Process32FirstW(snapshot, ref entry))
                throw new Win32Exception(Marshal.GetLastWin32Error());

            do
            {
                if (entry.th32ProcessID == processId)
                    return entry.th32ParentProcessID;
            }
            while (Process32NextW(snapshot, ref entry));
        }
        finally
        {
            CloseHandle(snapshot);
        }

        return 0;
    }

    public static string GetCommandLine(uint processId)
    {
        IntPtr process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, processId);
        if (process == IntPtr.Zero)
            return null;

        try
        {
            int length = 0;
            int status = NtQueryInformationProcess(
                process, ProcessCommandLineInformation, IntPtr.Zero, 0, out length);
            if (length <= 0)
                return null;

            IntPtr buffer = Marshal.AllocHGlobal(length);
            try
            {
                status = NtQueryInformationProcess(
                    process, ProcessCommandLineInformation, buffer, length, out length);
                if (status < 0)
                    return null;

                UNICODE_STRING commandLine =
                    (UNICODE_STRING)Marshal.PtrToStructure(buffer, typeof(UNICODE_STRING));
                if (commandLine.Buffer == IntPtr.Zero || commandLine.Length == 0)
                    return string.Empty;

                return Marshal.PtrToStringUni(commandLine.Buffer, commandLine.Length / 2);
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }
        finally
        {
            CloseHandle(process);
        }
    }

    public static DisplayModeInfo[] GetActiveDisplayModes()
    {
        var result = new System.Collections.Generic.List<DisplayModeInfo>();
        for (uint adapterIndex = 0; ; adapterIndex++)
        {
            DISPLAY_DEVICEW adapter = new DISPLAY_DEVICEW();
            adapter.cb = (uint)Marshal.SizeOf(typeof(DISPLAY_DEVICEW));
            if (!EnumDisplayDevicesW(null, adapterIndex, ref adapter, 0))
                break;
            if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0)
                continue;

            DEVMODEW mode = new DEVMODEW();
            mode.dmSize = (ushort)Marshal.SizeOf(typeof(DEVMODEW));
            if (!EnumDisplaySettingsW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, ref mode))
                continue;

            string pnpDeviceId = adapter.DeviceID;
            DISPLAY_DEVICEW monitor = new DISPLAY_DEVICEW();
            monitor.cb = (uint)Marshal.SizeOf(typeof(DISPLAY_DEVICEW));
            if (EnumDisplayDevicesW(adapter.DeviceName, 0, ref monitor, 0) &&
                !string.IsNullOrEmpty(monitor.DeviceID))
            {
                pnpDeviceId = monitor.DeviceID;
            }

            result.Add(new DisplayModeInfo
            {
                Name = adapter.DeviceString,
                PnpDeviceId = pnpDeviceId,
                DriverVersion = string.Empty,
                Width = mode.dmPelsWidth,
                Height = mode.dmPelsHeight,
                RefreshHz = mode.dmDisplayFrequency,
                VideoModeDescription =
                    mode.dmPelsWidth + " x " + mode.dmPelsHeight + " x " +
                    (mode.dmBitsPerPel < 64 ? (1UL << (int)mode.dmBitsPerPel) : 0UL) + " colors",
            });
        }
        return result.ToArray();
    }
}
'@
}

function Get-QuakeRayProcessAncestorIds {
    param([int]$ProcessId = $PID)

    $ancestors = @($ProcessId)
    try {
        $ancestorId = [int][QuakeRayMachineGuardProcessTree]::GetParentProcessId([uint32]$ProcessId)
        while ($ancestorId -gt 0 -and $ancestorId -notin $ancestors) {
            $ancestors += $ancestorId
            $ancestorId = [int][QuakeRayMachineGuardProcessTree]::GetParentProcessId([uint32]$ancestorId)
        }
    } catch {
        throw "MACHINE_GUARD_UNAVAILABLE: cannot inspect the process tree: $($_.Exception.Message)"
    }
    return $ancestors
}

function Get-QuakeRayActiveDisplays {
    return @([QuakeRayMachineGuardProcessTree]::GetActiveDisplayModes() |
        Where-Object { $_.Width -gt 0 -and $_.Height -gt 0 } |
        ForEach-Object {
            [ordered]@{
                Name = $_.Name
                PnpDeviceId = $_.PnpDeviceId
                DriverVersion = $_.DriverVersion
                Width = $_.Width
                Height = $_.Height
                RefreshHz = $_.RefreshHz
                VideoModeDescription = $_.VideoModeDescription
            }
        })
}

function Get-QuakeRayExternalRunnerProcesses {
    param([int[]]$ExcludedProcessIds = @())

    return @(Get-Process -Name powershell,pwsh -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -notin $ExcludedProcessIds } |
        ForEach-Object {
            $commandLine = [QuakeRayMachineGuardProcessTree]::GetCommandLine([uint32]$_.Id)
            if ($null -eq $commandLine) {
                throw "MACHINE_GUARD_UNAVAILABLE: cannot inspect runner command line for process $($_.Id)."
            }
            if ($commandLine -match 'run_(place|menu)\.ps1|build_win\.ps1') {
                [pscustomobject]@{
                    ProcessId = $_.Id
                    ProcessName = $_.ProcessName
                    CommandLine = $commandLine
                }
            }
        })
}

function ConvertFrom-QuakeRayGpuEngineCounters {
    param([AllowNull()][object[]]$Counters)

    $usageByProcess = @{}
    foreach ($counter in @($Counters)) {
        if ($null -eq $counter) { continue }
        $instance = if ($counter.InstanceName) { [string]$counter.InstanceName } else { [string]$counter.Name }
        if ($instance -notmatch '^pid_([0-9]+)_') { continue }
        $processId = [int]$Matches[1]
        $utilization = [double]$counter.UtilizationPercentage
        if (-not $usageByProcess.ContainsKey($processId)) {
            $usageByProcess[$processId] = [pscustomobject]@{
                ProcessId = $processId
                PeakUtilizationPercent = 0.0
                EngineCount = 0
            }
        }
        $usage = $usageByProcess[$processId]
        $usage.EngineCount++
        if ($utilization -gt $usage.PeakUtilizationPercent) {
            $usage.PeakUtilizationPercent = $utilization
        }
    }
    return @($usageByProcess.Values)
}

function Select-QuakeRayGpuBusyProcesses {
    param(
        [AllowNull()][object[]]$Usage,
        [ValidateRange(0.1, 100)][double]$MinimumUtilizationPercent = 10,
        [int[]]$ExcludedProcessIds = @(),
        [string[]]$IgnoredProcessNames = @('dwm')
    )

    $busy = foreach ($item in @($Usage)) {
        if ($null -eq $item -or $item.PeakUtilizationPercent -lt $MinimumUtilizationPercent) { continue }
        if ($item.ProcessId -in $ExcludedProcessIds) { continue }
        $process = Get-Process -Id $item.ProcessId -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $process -or $process.Threads.Count -eq 0) { continue }
        if ($process.ProcessName -in $IgnoredProcessNames) { continue }
        [pscustomobject]@{
            ProcessId = $item.ProcessId
            ProcessName = $process.ProcessName
            PeakUtilizationPercent = $item.PeakUtilizationPercent
            EngineCount = $item.EngineCount
        }
    }
    return @($busy)
}

function Get-QuakeRayGpuBusyProcesses {
    param(
        [ValidateRange(0.1, 100)][double]$MinimumUtilizationPercent = 10,
        [int[]]$ExcludedProcessIds = @(),
        [string[]]$IgnoredProcessNames = @('dwm'),
        [ValidateRange(1, 5)][int]$Samples = 2,
        [ValidateRange(0, 2000)][int]$SampleIntervalMilliseconds = 250,
        [scriptblock]$CounterProvider = {
            $sample = $null
            for ($attempt = 0; $attempt -lt 3; $attempt++) {
                try {
                    $sample = Get-Counter '\GPU Engine(*)\Utilization Percentage' -ErrorAction Stop
                    break
                } catch {
                    if ($attempt -eq 2) { throw }
                    Start-Sleep -Milliseconds 100
                }
            }
            foreach ($counter in $sample.CounterSamples) {
                [pscustomobject]@{
                    InstanceName = $counter.InstanceName
                    UtilizationPercentage = $counter.CookedValue
                }
            }
        }
    )

    $sustained = @{}
    for ($sample = 0; $sample -lt $Samples; $sample++) {
        try {
            $counters = @(& $CounterProvider)
        } catch {
            throw "MACHINE_GUARD_UNAVAILABLE: cannot inspect Windows GPU engine counters: $($_.Exception.Message)"
        }
        $activeThisSample = @{}
        foreach ($usage in @(ConvertFrom-QuakeRayGpuEngineCounters -Counters $counters)) {
            if ($usage.PeakUtilizationPercent -lt $MinimumUtilizationPercent) { continue }
            $activeThisSample[$usage.ProcessId] = $true
            if ($sustained.ContainsKey($usage.ProcessId)) {
                $sustained[$usage.ProcessId].Samples++
                if ($usage.PeakUtilizationPercent -gt $sustained[$usage.ProcessId].PeakUtilizationPercent) {
                    $sustained[$usage.ProcessId].PeakUtilizationPercent = $usage.PeakUtilizationPercent
                }
                if ($usage.EngineCount -gt $sustained[$usage.ProcessId].EngineCount) {
                    $sustained[$usage.ProcessId].EngineCount = $usage.EngineCount
                }
            } else {
                $sustained[$usage.ProcessId] = [pscustomobject]@{
                    ProcessId = $usage.ProcessId
                    PeakUtilizationPercent = $usage.PeakUtilizationPercent
                    EngineCount = $usage.EngineCount
                    Samples = 1
                }
            }
        }
        foreach ($processId in @($sustained.Keys)) {
            if (-not $activeThisSample.ContainsKey($processId)) {
                $sustained.Remove($processId)
            }
        }
        if ($sample + 1 -lt $Samples -and $SampleIntervalMilliseconds -gt 0) {
            Start-Sleep -Milliseconds $SampleIntervalMilliseconds
        }
    }

    $qualified = @($sustained.Values | Where-Object { $_.Samples -ge $Samples })
    return @(Select-QuakeRayGpuBusyProcesses -Usage $qualified `
        -MinimumUtilizationPercent $MinimumUtilizationPercent `
        -ExcludedProcessIds $ExcludedProcessIds `
        -IgnoredProcessNames $IgnoredProcessNames)
}

function Enter-QuakeRayMachine {
    param([ValidateRange(0, 300)][int]$TimeoutSeconds = 5)
    $mutex = [System.Threading.Mutex]::new($false, 'Local\QuakeRayPerformanceRun')
    $locked = $false
    try {
        try { $locked = $mutex.WaitOne([TimeSpan]::FromSeconds($TimeoutSeconds)) }
        catch [System.Threading.AbandonedMutexException] { $locked = $true }
        if (-not $locked) { throw 'MACHINE_BUSY: QuakeRay machine guard is occupied; defer this operation.' }
        # A terminated process keeps its entry visible to Get-Process while another
        # handle to it stays open; such an entry owns no threads and cannot contend
        # with a measurement run.
        $busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
            $_.ProcessName -match '^(quakeray|qray_|cmake$|ninja$|cl$|link$)' -and
            $_.Threads.Count -gt 0
        })
        if ($busy.Count) { throw "MACHINE_BUSY: foreign game/build processes are running: $($busy.Id -join ', ')." }
        $ancestors = @(Get-QuakeRayProcessAncestorIds)
        $runners = @(Get-QuakeRayExternalRunnerProcesses -ExcludedProcessIds $ancestors)
        if ($runners.Count) { throw 'MACHINE_BUSY: a non-cooperating runner/build owns the machine.' }
        $gpuBusy = @(Get-QuakeRayGpuBusyProcesses -ExcludedProcessIds $ancestors)
        if ($gpuBusy.Count) {
            $owners = ($gpuBusy | ForEach-Object {
                "$($_.ProcessId):$($_.ProcessName)=$([math]::Round($_.PeakUtilizationPercent, 1))%"
            }) -join ', '
            throw "MACHINE_BUSY: foreign sustained GPU activity is running: $owners."
        }
        return $mutex
    } catch {
        if ($locked) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
        throw
    }
}

function Exit-QuakeRayMachine {
    param($Guard)
    if ($null -ne $Guard) {
        $Guard.ReleaseMutex()
        $Guard.Dispose()
    }
}

function Test-QuakeRayStopRequested {
    return $env:QUAKERAY_JOB_STOP_FILE -and (Test-Path -LiteralPath $env:QUAKERAY_JOB_STOP_FILE)
}
