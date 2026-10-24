/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * The menu of the CPU graph, like ProcessController's: quit an application, the memory in use by each program, and every process
 * with what it takes of the processor, each with its threads, their priorities, and Force Kill. The rows follow the system while
 * the menu is open.
 */

#include "cpumenu.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>

#include "alert.h"
#include "apps.h"
#include "util.h"

namespace {

// ---- what the system says (/proc) ----

struct MemInfo {
    uint64_t totalKb = 0, availableKb = 0, freeKb = 0, cachedKb = 0;
    static MemInfo Read() {
        MemInfo m;
        std::string s = ReadFile("/proc/meminfo");
        auto field = [&](const char* name) -> uint64_t {
            size_t p = s.find(name);
            if (p == std::string::npos) return 0;
            return strtoull(s.c_str() + p + strlen(name), nullptr, 10);
        };
        m.totalKb = field("MemTotal:");
        m.availableKb = field("MemAvailable:");
        m.freeKb = field("MemFree:");
        m.cachedKb = field("Cached:") + field("Buffers:");
        return m;
    }
};

struct ProcessInfo {
    pid_t pid = 0;
    std::string name;
    std::string exe;
    uid_t uid = 0;
    uint64_t cpuTicks = 0;
    double cpuPercent = 0;
};

// A process's memory in KiB from /proc/<pid>/status: all the RAM it has (VmRSS), and what it has written to (private and
// shared anonymous pages: RssAnon + RssShmem), the pair ProcessController shows as "all" and "written". false for a process
// that is gone or has no memory (a kernel thread).
static bool ReadProcessMemory(pid_t pid, long long* allKb, long long* writtenKb) {
    std::string s = ReadFile("/proc/" + std::to_string(pid) + "/status");
    auto field = [&](const char* name) -> long long {
        size_t p = s.find(name);
        return p == std::string::npos ? -1 : strtoll(s.c_str() + p + strlen(name), nullptr, 10);
    };
    long long all = field("VmRSS:");
    if (all < 0) return false;
    long long anon = field("RssAnon:"), shmem = field("RssShmem:");
    *allKb = all;
    *writtenKb = std::max(0LL, anon) + std::max(0LL, shmem);
    return true;
}

struct ThreadInfo {
    pid_t tid = 0;
    std::string name;
    char state = '?';
    double cpuPercent = 0;
};

// One process's threads, from /proc/<pid>/task/<tid>/{comm,stat} -- the same
// source `top -H` uses. CPU% is measured since the previous call, on the same
// scale as ProcessSampler (share of all CPUs).
class ThreadSampler {
public:
    explicit ThreadSampler(pid_t pid) : fPid(pid) {}

    std::vector<ThreadInfo> Sample() {
        std::vector<ThreadInfo> out;
        uint64_t nowMs = NowMs();
        double elapsedTicks = (fLastMs == 0) ? 0 : (nowMs - fLastMs) / 1000.0 * sysconf(_SC_CLK_TCK);
        long cpus = std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
        std::map<pid_t, uint64_t> current;
        std::string taskDir = "/proc/" + std::to_string(fPid) + "/task";
        DIR* d = opendir(taskDir.c_str());
        if (d == nullptr) return out;
        while (struct dirent* e = readdir(d)) {
            if (!isdigit(static_cast<unsigned char>(e->d_name[0]))) continue;
            ThreadInfo t;
            t.tid = atoi(e->d_name);
            std::string base = taskDir + "/" + e->d_name;
            std::string stat = ReadFile(base + "/stat");
            size_t lp = stat.find('('), rp = stat.rfind(')');
            if (lp == std::string::npos || rp == std::string::npos || rp + 2 >= stat.size()) continue;
            t.name = Trim(ReadFile(base + "/comm"));
            if (t.name.empty()) t.name = stat.substr(lp + 1, rp - lp - 1);
            // After ")": state(3) ... utime(14) stime(15)
            std::vector<std::string> fields;
            size_t pos = rp + 2;
            while (pos < stat.size() && fields.size() < 14) {
                size_t sp = stat.find(' ', pos);
                if (sp == std::string::npos) sp = stat.size();
                fields.push_back(stat.substr(pos, sp - pos));
                pos = sp + 1;
            }
            if (fields.size() < 13) continue;
            t.state = fields[0].empty() ? '?' : fields[0][0];
            uint64_t ticks = strtoull(fields[11].c_str(), nullptr, 10) + strtoull(fields[12].c_str(), nullptr, 10);
            auto prev = fPrev.find(t.tid);
            if (prev != fPrev.end() && elapsedTicks > 0 && ticks >= prev->second) {
                t.cpuPercent = (ticks - prev->second) / (elapsedTicks * cpus) * 100.0;
            }
            current[t.tid] = ticks;
            out.push_back(t);
        }
        closedir(d);
        fPrev = std::move(current);
        fLastMs = nowMs;
        return out;
    }

private:
    pid_t fPid;
    std::map<pid_t, uint64_t> fPrev;
    uint64_t fLastMs = 0;
};

class ProcessSampler {
public:
    // All processes, with CPU% measured since the previous call (the
    // BRealtimeCpuMenu pulse equivalent).
    std::vector<ProcessInfo> Sample() {
        std::vector<ProcessInfo> out;
        uint64_t nowMs = NowMs();
        double elapsedTicks = (fLastMs == 0) ? 0 : (nowMs - fLastMs) / 1000.0 * sysconf(_SC_CLK_TCK);
        long cpus = std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
        std::map<pid_t, uint64_t> current;
        DIR* d = opendir("/proc");
        if (d == nullptr) return out;
        while (struct dirent* e = readdir(d)) {
            if (!isdigit(static_cast<unsigned char>(e->d_name[0]))) continue;
            ProcessInfo p;
            p.pid = atoi(e->d_name);
            std::string base = std::string("/proc/") + e->d_name;
            std::string stat = ReadFile(base + "/stat");
            size_t lp = stat.find('('), rp = stat.rfind(')');
            if (lp == std::string::npos || rp == std::string::npos) continue;
            p.name = stat.substr(lp + 1, rp - lp - 1);
            // Fields after ")": state(3) ... utime(14) stime(15)
            std::vector<std::string> fields;
            size_t pos = rp + 2;
            while (pos < stat.size() && fields.size() < 14) {
                size_t sp = stat.find(' ', pos);
                if (sp == std::string::npos) sp = stat.size();
                fields.push_back(stat.substr(pos, sp - pos));
                pos = sp + 1;
            }
            if (fields.size() < 13) continue;
            // Kernel threads have no memory map; skip them like Haiku skips "kernel".
            if (!FileExists(base + "/exe") && ReadFile(base + "/cmdline").empty()) continue;
            p.cpuTicks = strtoull(fields[11].c_str(), nullptr, 10) + strtoull(fields[12].c_str(), nullptr, 10);
            struct stat st;
            if (::stat(base.c_str(), &st) == 0) p.uid = st.st_uid;
            char exe[PATH_MAX];
            ssize_t n = readlink((base + "/exe").c_str(), exe, sizeof(exe) - 1);
            if (n > 0) { exe[n] = '\0'; p.exe = exe; }
            auto prev = fPrev.find(p.pid);
            if (prev != fPrev.end() && elapsedTicks > 0 && p.cpuTicks >= prev->second) {
                p.cpuPercent = (p.cpuTicks - prev->second) / (elapsedTicks * cpus) * 100.0;
            }
            current[p.pid] = p.cpuTicks;
            out.push_back(p);
        }
        closedir(d);
        fPrev = std::move(current);
        fLastMs = nowMs;
        return out;
    }

private:
    std::map<pid_t, uint64_t> fPrev;
    uint64_t fLastMs = 0;
};


std::string IconForExecutable(const std::string& exe)
{
	AppEntry* entry = (exe.empty() || gApps == nullptr) ? nullptr : gApps->ResolveExecutable(AppDatabase::Basename(exe));
	return entry != nullptr ? entry->iconName : "application-x-executable";
}

// Programs the session depends on: no "Quit" for them in the list.
bool IsSessionCritical(const std::string& name)
{
	static const char* kProtected[] = {
		"systemd", "dbus-daemon", "dbus-broker", "dbus-broker-launch", "pipewire", "pipewire-pulse",
		"wireplumber", "kwin_wayland", "kwin_wayland_wrapper", "Hyprland", "sway", "niri", "labwc",
		"plasmashell", "ksmserver", "kded6", "Xwayland", "xdg-desktop-portal", "gnome-keyring-daemon",
		"kwalletd6", "polkit-kde-authentication-agent-1", "startplasma-wayland", "sddm", "sddm-helper",
		"hdeskbar", "hdesktop", "hdesktop_linux", "(sd-pam)", "at-spi-bus-launcher", "at-spi2-registryd",
	};
	for (const char* protectedName : kProtected) {
		if (name == protectedName)
			return true;
	}
	return g_str_has_prefix(name.c_str(), "xdg-desktop-portal") || g_str_has_prefix(name.c_str(), "xdg-");
}

std::vector<MenuItem> QuitApplicationItems()
{
	ProcessSampler sampler;
	const std::vector<ProcessInfo> processes = sampler.Sample();
	const uid_t me = getuid();
	std::map<std::string, std::vector<pid_t> > byName;
	std::map<std::string, std::string> iconFor;
	for (const ProcessInfo& p : processes) {
		if (p.uid != me || p.exe.empty() || p.pid == getpid())
			continue;
		const std::string name = AppDatabase::Basename(p.exe);
		if (IsSessionCritical(name) || IsSessionCritical(p.name))
			continue;
		byName[name].push_back(p.pid);
		if (!iconFor.count(name))
			iconFor[name] = IconForExecutable(p.exe);
	}
	std::vector<MenuItem> items;
	for (const auto& entry : byName) {
		MenuItem item;
		item.label = entry.first;
		item.iconName = iconFor[entry.first];
		const std::string name = entry.first;
		const std::vector<pid_t> pids = entry.second;
		item.action = [name, pids]() {
			ShowAlert("Close Application",
				"Do you want to close '" + name + "' smoothly?\n\n"
				"This will send a standard termination request (SIGTERM) to the application.",
				{"Cancel", "Close App"}, [pids](int choice) {
					if (choice == 1) {
						for (pid_t pid : pids)
							kill(pid, SIGTERM);
					}
				}, 1);
		};
		items.push_back(item);
	}
	if (items.empty()) {
		MenuItem none;
		none.label = "No applications running";
		none.enabled = false;
		items.push_back(none);
	}
	return items;
}

// The Memory usage menu: "System resources & caches", then a row for every process: its icon and name, what it has written to and
// all the RAM it has, and a bar of the two against the memory in use (written: dark blue, the rest: light blue). The system row's
// bar is what is cached (dark blue), what else is in use (lavender) and what is free.
std::vector<MenuItem> MemoryItems()
{
	const MemInfo memory = MemInfo::Read();
	const long long physical = static_cast<long long>(memory.totalKb);
	const long long inUse = physical - static_cast<long long>(memory.freeKb);
	const long long cached = static_cast<long long>(memory.cachedKb);
	std::vector<MenuItem> items;

	MenuItem system;
	system.label = "System resources & caches\u2026";
	system.iconName = "computer";
	system.memRow = true;
	system.memSystem = true;
	system.memFirstKb = inUse;
	system.memSecondKb = cached;
	if (physical > 0) {
		system.memBarFirst = static_cast<double>(cached) / physical;
		system.memBarSecond = static_cast<double>(inUse) / physical;
	}
	system.action = []() {};
	items.push_back(system);

	struct Row {
		MenuItem item;
		long long all;
	};
	std::vector<Row> rows;
	ProcessSampler sampler;
	for (const ProcessInfo& p : sampler.Sample()) {
		long long all = 0, written = 0;
		if (!ReadProcessMemory(p.pid, &all, &written))
			continue;
		MenuItem row;
		row.label = p.name;
		row.iconName = IconForExecutable(p.exe);
		row.memRow = true;
		row.memFirstKb = written;
		row.memSecondKb = all;
		if (inUse > 0) {
			row.memBarFirst = static_cast<double>(written) / inUse;
			row.memBarSecond = static_cast<double>(all) / inUse;
		}
		row.action = []() {};
		rows.push_back({ row, all });
	}
	std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.all > b.all; });
	for (const Row& row : rows)
		items.push_back(row.item);
	return items;
}

// Whether this user may raise priorities and touch other users' threads: root, or CAP_SYS_NICE. Without it the kernel only lets a
// thread's nice value go up (lower priority), down to 20 - RLIMIT_NICE at most.
bool HasNiceCapability()
{
	if (geteuid() == 0)
		return true;
	const std::string status = ReadFile("/proc/self/status");
	const size_t at = status.find("CapEff:");
	if (at == std::string::npos)
		return false;
	const unsigned long long caps = strtoull(status.c_str() + at + 7, nullptr, 16);
	return (caps >> 23) & 1;
}

int LowestNiceAllowed()
{
	if (HasNiceCapability())
		return -20;
	struct rlimit limit;
	if (getrlimit(RLIMIT_NICE, &limit) != 0)
		return 20;
	if (limit.rlim_cur == RLIM_INFINITY)
		return -20;
	return std::clamp(20 - static_cast<int>(limit.rlim_cur), -20, 20);
}

// The priority menu of a thread, in Linux terms: nice values from 19 (lowest priority) to -20 (highest), the current one chosen.
// What the kernel would refuse is greyed out.
std::vector<MenuItem> ThreadPriorityItems(pid_t tid, bool mine)
{
	std::vector<MenuItem> items;
	errno = 0;
	const int current = getpriority(PRIO_PROCESS, static_cast<id_t>(tid));
	if (errno != 0) {
		items.push_back(MenuItem::Header("Thread has exited"));
		return items;
	}
	const bool privileged = HasNiceCapability();
	const bool canChange = mine || privileged;
	const int lowestAllowed = LowestNiceAllowed();
	static const struct {
		const char* name;
		int nice;
	} kLevels[] = {
		{"Lowest priority", 19}, {"Low priority", 10}, {"Below normal priority", 5}, {"Normal priority", 0},
		{"Above normal priority", -5}, {"High priority", -10}, {"Highest priority", -20},
	};
	bool anyBlocked = false;
	auto add = [&](const std::string& label, int nice) {
		MenuItem item;
		item.label = label + " [nice " + std::to_string(nice) + "]";
		item.check = (nice == current) ? kRadioOn : kRadioOff;
		item.enabled = canChange && (nice >= current || nice >= lowestAllowed);
		if (!item.enabled && nice != current)
			anyBlocked = true;
		item.action = [tid, nice]() {
			if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice) != 0) {
				ShowAlert("Couldn't Change Priority", "Setting thread " + std::to_string(tid) + " to nice " + std::to_string(nice)
					+ " failed: " + strerror(errno) + ".", {"OK"}, nullptr, 0);
			}
		};
		items.push_back(item);
	};
	bool named = false;
	for (const auto& level : kLevels)
		named = named || level.nice == current;
	bool customAdded = named;
	for (const auto& level : kLevels) {
		if (!customAdded && current > level.nice) {
			add("Custom priority", current);
			customAdded = true;
		}
		add(level.name, level.nice);
	}
	if (!customAdded)
		add("Custom priority", current);
	if (!canChange) {
		items.push_back(MenuItem::Separator());
		items.push_back(MenuItem::Header("Not your process"));
	} else if (anyBlocked) {
		items.push_back(MenuItem::Separator());
		items.push_back(MenuItem::Header("Raising priority needs root"));
	}
	return items;
}

// One process: Force Kill, and its threads, each with its priority menu. A single thread can't be killed (Linux has no way to do
// that from outside a process, and the program would be left broken): the kill is of the whole process.
std::vector<MenuItem> ThreadItems(pid_t pid, const std::string& name, bool mine, ThreadSampler& sampler)
{
	std::vector<MenuItem> items;
	MenuItem killItem;
	killItem.label = mine ? "Force Kill \u201c" + name + "\u201d\u2026" : "Force Kill (not your process)";
	killItem.iconName = "process-stop";
	killItem.enabled = mine;
	killItem.action = [pid, name]() {
		ShowAlert("Force Terminate",
			"Are you sure you want to force terminate the process '" + name + "' (PID: " + std::to_string(pid) + ")?\n\n"
			"Unsaved progress inside this application will be lost.",
			{"Cancel", "Force Kill"}, [pid](int choice) {
				if (choice == 1)
					kill(pid, SIGKILL);
			}, 0);
	};
	items.push_back(killItem);
	items.push_back(MenuItem::Separator());

	std::vector<ThreadInfo> threads = sampler.Sample();
	std::stable_sort(threads.begin(), threads.end(), [](const ThreadInfo& a, const ThreadInfo& b) {
		if (a.cpuPercent != b.cpuPercent)
			return a.cpuPercent > b.cpuPercent;
		return a.tid < b.tid;
	});
	if (threads.empty()) {
		items.push_back(MenuItem::Header("No threads (process has exited)"));
		return items;
	}
	char header[96];
	snprintf(header, sizeof(header), "Threads: %zu", threads.size());
	items.push_back(MenuItem::Header(header));
	for (const ThreadInfo& t : threads) {
		MenuItem row;
		row.label = t.name + "  (" + std::to_string(t.tid) + ")";
		row.barPercent = t.cpuPercent;
		char text[16];
		snprintf(text, sizeof(text), "%3.1f%%", t.cpuPercent);
		row.valueText = text;
		const pid_t tid = t.tid;
		row.submenu = [tid, mine]() { return ThreadPriorityItems(tid, mine); };
		row.noArrow = true;
		items.push_back(row);
	}
	return items;
}

// Every process, the busiest first, each with its threads.
std::vector<MenuItem> ProcessItems(ProcessSampler& sampler)
{
	std::vector<ProcessInfo> processes = sampler.Sample();
	std::sort(processes.begin(), processes.end(), [](const ProcessInfo& a, const ProcessInfo& b) { return a.cpuPercent > b.cpuPercent; });
	std::vector<MenuItem> items;
	for (const ProcessInfo& p : processes) {
		MenuItem item;
		item.label = p.name;
		item.barPercent = p.cpuPercent;
		char text[16];
		snprintf(text, sizeof(text), "%3.1f%%", p.cpuPercent);
		item.valueText = text;
		item.iconName = IconForExecutable(p.exe);
		const pid_t pid = p.pid;
		const std::string name = p.name;
		const bool mine = p.uid == getuid();
		// sampled only once the submenu opens: the first pass gives the figures to measure from
		auto threads = std::make_shared<ThreadSampler>(pid);
		item.submenu = [pid, name, mine, threads]() { return ThreadItems(pid, name, mine, *threads); };
		item.noArrow = true;
		item.liveMs = 1000;
		items.push_back(item);
	}
	return items;
}

std::string CurrentPowerProfile()
{
	char* program = g_find_program_in_path("powerprofilesctl");
	if (program == nullptr)
		return std::string();
	g_free(program);
	char* out = nullptr;
	if (!g_spawn_command_line_sync("powerprofilesctl get", &out, nullptr, nullptr, nullptr))
		return std::string();
	const std::string profile = Trim(out != nullptr ? out : "");
	g_free(out);
	return profile;
}

}	// namespace

std::vector<MenuItem> CpuMenuItems()
{
	std::vector<MenuItem> items;

	MenuItem quitApplications;
	quitApplications.label = "Quit an application";
	quitApplications.iconName = "application-exit";
	quitApplications.submenu = QuitApplicationItems;
	quitApplications.noArrow = true;
	items.push_back(quitApplications);

	MenuItem memory;
	memory.label = "Memory usage";
	memory.iconName = "memory";
	memory.submenu = MemoryItems;
	memory.liveMs = 500;
	memory.noArrow = true;
	items.push_back(memory);

	MenuItem processes;
	processes.label = "Processes and CPU usage";
	processes.iconName = "utilities-system-monitor";
	auto sampler = std::make_shared<ProcessSampler>();
	sampler->Sample();		// (the figures are the change since the last sample: this is the first)
	processes.submenu = [sampler]() { return ProcessItems(*sampler); };
	processes.liveMs = 1000;
	processes.noArrow = true;
	items.push_back(processes);

	const std::string profile = CurrentPowerProfile();
	if (!profile.empty()) {
		MenuItem power;
		power.label = "Power saving";
		const bool saving = profile == "power-saver";
		power.check = saving ? kCheckOn : kCheckOff;
		power.action = [saving]() {
			GError* error = nullptr;
			g_spawn_command_line_async(saving ? "powerprofilesctl set balanced" : "powerprofilesctl set power-saver", &error);
			g_clear_error(&error);
		};
		items.push_back(power);
	}
	return items;
}
