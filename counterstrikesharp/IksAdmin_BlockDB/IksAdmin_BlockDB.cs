using System;
using System.IO;
using System.Linq.Expressions;
using System.Reflection;
using System.Runtime.InteropServices;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Capabilities;

namespace IksAdmin_BlockDB;

public class IksAdmin_BlockDB : BasePlugin
{
    public override string ModuleName    => "IksAdmin_BlockDB";
    public override string ModuleVersion => "1.0.0";
    public override string ModuleAuthor  => "_ded_cookies";

    private const string NATIVE = "blockdb";

    [DllImport(NATIVE, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern void BlockDB_RelayBan(
        [MarshalAs(UnmanagedType.LPStr)] string victimSid,
        [MarshalAs(UnmanagedType.LPStr)] string victimName,
        [MarshalAs(UnmanagedType.LPStr)] string victimIp,
        [MarshalAs(UnmanagedType.LPStr)] string adminSid,
        [MarshalAs(UnmanagedType.LPStr)] string adminName,
        int durationMinutes,
        [MarshalAs(UnmanagedType.LPStr)] string reason);

    [DllImport(NATIVE, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern void BlockDB_RelayUnban(
        [MarshalAs(UnmanagedType.LPStr)] string victimSid);

    static IksAdmin_BlockDB()
    {
        NativeLibrary.SetDllImportResolver(typeof(IksAdmin_BlockDB).Assembly, ResolveNative);
    }

    private static IntPtr ResolveNative(string name, Assembly asm, DllImportSearchPath? path)
    {
        if (name != NATIVE) return IntPtr.Zero;

        string gameDir = Server.GameDirectory;
        string[] candidates = {
            Path.Combine(gameDir, "csgo",   "addons", "admin_modules", "blockdb.so"),
            Path.Combine(gameDir, "csgo",   "addons", "admin_modules", "blockdb.dll"),
            Path.Combine(gameDir, "addons", "admin_modules", "blockdb.so"),
            Path.Combine(gameDir, "addons", "admin_modules", "blockdb.dll"),
        };
        foreach (string p in candidates)
        {
            if (File.Exists(p) && NativeLibrary.TryLoad(p, out IntPtr h))
                return h;
        }
        return IntPtr.Zero;
    }

    private bool _hooked = false;
    private bool _gaveUp = false;
    private int  _attempts = 0;
    private const int MaxAttempts = 15;

    public override void OnAllPluginsLoaded(bool hotReload)
    {
        if (TryHookApi()) return;

        AddTimer(1.0f, () =>
        {
            if (_hooked || _gaveUp) return;

            _attempts++;
            if (TryHookApi()) return;

            if (_attempts >= MaxAttempts) _gaveUp = true;
        }, CounterStrikeSharp.API.Modules.Timers.TimerFlags.REPEAT);
    }

    private object? TryGetApiViaRealType()
    {
        Type? iksApiType = null;
        foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
        {
            try
            {
                var t = asm.GetType("IksAdminApi.IIksAdminApi", throwOnError: false);
                if (t != null) { iksApiType = t; break; }
            }
            catch { }
        }
        if (iksApiType == null) return null;

        try
        {
            var capType  = typeof(PluginCapability<>).MakeGenericType(iksApiType);
            var capInst  = Activator.CreateInstance(capType, "iksadmin:core");
            var getMeth  = capType.GetMethod("Get");
            return getMeth?.Invoke(capInst, null);
        }
        catch
        {
            return null;
        }
    }

    private object? TryGetApiAsObject()
    {
        try
        {
            return new PluginCapability<object>("iksadmin:core").Get();
        }
        catch
        {
            return null;
        }
    }

    private bool TryHookApi()
    {
        object? api = TryGetApiViaRealType() ?? TryGetApiAsObject();
        if (api == null) return false;

        var apiType = api.GetType();

        var evBanV3   = apiType.GetEvent("OnBanPost");
        var evBanV2   = apiType.GetEvent("OnAddBan");
        var evUnbanV3 = apiType.GetEvent("OnUnBanPost");
        var evUnbanV2 = apiType.GetEvent("OnUnBan");

        if (evBanV3 != null)
        {
            HookBanV3(api, evBanV3);
            if (evUnbanV3 != null) HookUnbanV3(api, evUnbanV3);
            Console.WriteLine("[IksAdmin_BlockDB] Detected Iks_Admin v3");
            _hooked = true;
            return true;
        }
        if (evBanV2 != null)
        {
            HookBanV2(api, evBanV2);
            if (evUnbanV2 != null) HookUnbanV2(api, evUnbanV2);
            Console.WriteLine("[IksAdmin_BlockDB] Detected Iks_Admin v2");
            _hooked = true;
            return true;
        }

        _hooked = true;
        return true;
    }

    // ----------------- v3 hook ------------------

    private void HookBanV3(object api, EventInfo ev)
    {
        Type handlerType = ev.EventHandlerType!;
        MethodInfo invoke = handlerType.GetMethod("Invoke")!;
        Type playerBanType = invoke.GetParameters()[0].ParameterType;

        var banParam      = Expression.Parameter(playerBanType, "ban");
        var announceParam = Expression.Parameter(invoke.GetParameters()[1].ParameterType, "announce");

        var thisConst = Expression.Constant(this);
        var call = Expression.Call(thisConst,
            typeof(IksAdmin_BlockDB).GetMethod(nameof(RelayBanFromObject),
                BindingFlags.NonPublic | BindingFlags.Instance)!,
            Expression.Convert(banParam, typeof(object)),
            Expression.Constant(true));

        var hookResultType = invoke.ReturnType;
        var continueValue  = Expression.Constant(Enum.ToObject(hookResultType, 0), hookResultType);

        var body = Expression.Block(call, continueValue);
        var lambda = Expression.Lambda(handlerType, body, banParam, announceParam);

        ev.AddEventHandler(api, lambda.Compile());
    }

    private void HookUnbanV3(object api, EventInfo ev)
    {
        Type handlerType = ev.EventHandlerType!;
        MethodInfo invoke = handlerType.GetMethod("Invoke")!;
        var ps = invoke.GetParameters();

        var p0 = Expression.Parameter(ps[0].ParameterType, "admin");
        var p1 = Expression.Parameter(ps[1].ParameterType, "arg");
        var p2 = Expression.Parameter(ps[2].ParameterType, "reason");
        var p3 = Expression.Parameter(ps[3].ParameterType, "announce");

        var thisConst = Expression.Constant(this);
        var call = Expression.Call(thisConst,
            typeof(IksAdmin_BlockDB).GetMethod(nameof(RelayUnbanFromString),
                BindingFlags.NonPublic | BindingFlags.Instance)!,
            p1);

        var hookResultType = invoke.ReturnType;
        var continueValue  = Expression.Constant(Enum.ToObject(hookResultType, 0), hookResultType);

        var body = Expression.Block(call, continueValue);
        var lambda = Expression.Lambda(handlerType, body, p0, p1, p2, p3);

        ev.AddEventHandler(api, lambda.Compile());
    }

    // ----------------- v2 hook ------------------

    private void HookBanV2(object api, EventInfo ev)
    {
        Type handlerType = ev.EventHandlerType!;
        MethodInfo invoke = handlerType.GetMethod("Invoke")!;
        Type playerBanType = invoke.GetParameters()[0].ParameterType;

        var banParam = Expression.Parameter(playerBanType, "ban");

        var thisConst = Expression.Constant(this);
        var call = Expression.Call(thisConst,
            typeof(IksAdmin_BlockDB).GetMethod(nameof(RelayBanFromObject),
                BindingFlags.NonPublic | BindingFlags.Instance)!,
            Expression.Convert(banParam, typeof(object)),
            Expression.Constant(false));

        var lambda = Expression.Lambda(handlerType, call, banParam);
        ev.AddEventHandler(api, lambda.Compile());
    }

    private void HookUnbanV2(object api, EventInfo ev)
    {
        Type handlerType = ev.EventHandlerType!;
        MethodInfo invoke = handlerType.GetMethod("Invoke")!;
        Type playerBanType = invoke.GetParameters()[0].ParameterType;

        var banParam = Expression.Parameter(playerBanType, "ban");

        var thisConst = Expression.Constant(this);
        var call = Expression.Call(thisConst,
            typeof(IksAdmin_BlockDB).GetMethod(nameof(RelayUnbanFromObject),
                BindingFlags.NonPublic | BindingFlags.Instance)!,
            Expression.Convert(banParam, typeof(object)));

        var lambda = Expression.Lambda(handlerType, call, banParam);
        ev.AddEventHandler(api, lambda.Compile());
    }

    // ----------------- helpers ------------------

    private static string? GetProp(object obj, string name)
    {
        var p = obj.GetType().GetProperty(name);
        if (p == null) return null;
        var v = p.GetValue(obj);
        return v?.ToString();
    }

    private static int GetIntProp(object obj, string name, int def = 0)
    {
        var p = obj.GetType().GetProperty(name);
        if (p == null) return def;
        var v = p.GetValue(obj);
        if (v is int i)      return i;
        if (v is long l)     return (int)l;
        if (v is short s)    return s;
        if (v is sbyte sb)   return sb;
        if (v is byte b)     return b;
        return def;
    }

    private void RelayBanFromObject(object ban, bool isV3)
    {
        string victimSid = isV3 ? (GetProp(ban, "SteamId") ?? "") : (GetProp(ban, "Sid") ?? "");
        if (string.IsNullOrEmpty(victimSid) || victimSid == "0") return;

        string victimName = GetProp(ban, "Name") ?? "Unknown";
        string victimIp   = GetProp(ban, "Ip")   ?? "";
        int colonIdx = victimIp.IndexOf(':');
        if (colonIdx > 0) victimIp = victimIp.Substring(0, colonIdx);

        string? reasonRaw = GetProp(ban, "Reason");
        string  reason    = string.IsNullOrEmpty(reasonRaw) ? "no reason" : reasonRaw;

        int durationSec = isV3 ? GetIntProp(ban, "Duration") : GetIntProp(ban, "Time");
        int duration = durationSec == 0 ? 0 : durationSec / 60;

        string adminSid;
        string adminName;
        if (isV3)
        {
            var adminProp = ban.GetType().GetProperty("Admin");
            var admin     = adminProp?.GetValue(ban);
            adminSid  = (admin != null ? GetProp(admin, "SteamId") : null) ?? "0";
            adminName = (admin != null ? GetProp(admin, "Name")    : null) ?? "Console";
            if (string.IsNullOrEmpty(adminSid) || adminSid == "0") adminSid = "0";
        }
        else
        {
            adminSid  = GetProp(ban, "AdminSid") ?? "0";
            adminName = GetProp(ban, "AdminName") ?? "Console";
            if (adminSid.Equals("CONSOLE", StringComparison.OrdinalIgnoreCase) || adminSid == "")
                adminSid = "0";
        }

        Server.NextFrame(() =>
        {
            try
            {
                BlockDB_RelayBan(victimSid, victimName, victimIp,
                                 adminSid, adminName, duration, reason);
            }
            catch { }
        });
    }

    private void RelayUnbanFromObject(object ban)
    {
        string sid = GetProp(ban, "Sid") ?? GetProp(ban, "SteamId") ?? "";
        RelayUnbanFromString(sid);
    }

    private void RelayUnbanFromString(string? arg)
    {
        if (string.IsNullOrEmpty(arg)) return;
        if (!ulong.TryParse(arg, out ulong sid) || sid == 0) return;
        string s = sid.ToString();

        Server.NextFrame(() =>
        {
            try
            {
                BlockDB_RelayUnban(s);
            }
            catch { }
        });
    }
}
