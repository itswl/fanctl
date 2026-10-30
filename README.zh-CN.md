# fanctl

Apple Silicon Mac 的风扇温控：一个小小的 C 程序，读温度传感器，直接把目标转速写进 SMC。
不用装 App，也不用内核扩展。

[English](README.md)

## 安装

要求：Apple Silicon Mac，macOS 14 或更高。在终端里：

```sh
curl -fsSL https://github.com/itswl/fanctl/releases/latest/download/install.sh | sh
```

或者用 wget：

```sh
wget -qO- https://github.com/itswl/fanctl/releases/latest/download/install.sh | sh
```

安装脚本是单个文件，自带编译好的二进制，不用克隆仓库，也不用编译。它会要你输密码，装好
`fanctl`、预设、配置（**balanced** 预设）和 LaunchDaemon，启动并打印状态。再跑一次同样的命令
就是升级，已有的配置和开机自启设置都会保留。想先看看脚本的话，下载 `install.sh` 再 `sh install.sh`。

想一开始就用别的预设（`quiet`、`balanced`、`cool`、`lap`），加上 `-s -- --preset 名字`，
旧配置会保存为 `fancontrol.conf.bak`：

```sh
curl -fsSL https://github.com/itswl/fanctl/releases/latest/download/install.sh | sh -s -- --preset lap
```

卸载：`sudo /usr/local/share/fanctl/uninstall.sh`（加 `--purge` 连配置和日志一起删），
风扇交回 macOS。

## 预设

| 预设 | 接管温度 | 适合 |
|---|---|---|
| `balanced`（默认） | 55 °C | 日常。轻负载交给 macOS，风扇可以停转；持续负载时风扇起得早一点、转得快一点。 |
| `quiet` | 62 °C | 更安静。62 °C 以下风扇全交给 macOS；长时间满载会高几度。 |
| `cool` | 48 °C | 长时间编译、渲染、跑模型。中等负载就听得到风扇。 |
| `lap` | 40 °C | 放腿上用：风扇一直转，压住底壳温度。待机也有轻微风噪。 |

`sudo fanctl preset quiet` 切换预设，立刻生效；如果配置是你手改过的，会先存成
`fancontrol.conf.bak`。`fanctl preset` 列出所有预设，并标出当前用的是哪个。

## 用法

```sh
fanctl status                    # 温度、风扇（手动/自动）、预设、守护进程、开机自启
fanctl preset                    # 列出预设（* 是当前的）
sudo fanctl preset quiet         # 切换预设，立刻生效
sudo fanctl stop                 # 现在停止，风扇交回 macOS
sudo fanctl start                # 再启动
sudo fanctl autostart off        # 开机不自启（on：开机自启，默认）
sudo fanctl reload               # 改完 /usr/local/etc/fancontrol.conf 之后
fanctl temp                      # 曲线用的 PMU 温度
fanctl run --dry-run --verbose   # 看守护进程会怎么做，不碰风扇
```

`stop` 和 `start` 只管现在；开机要不要自动启动由 `autostart` 决定。

### 手动控制

守护进程在跑的话会覆盖手动设置，所以先停掉它，用完再启动：

```sh
sudo fanctl stop          # 停掉守护进程（风扇先交回 macOS）
sudo fanctl set 3000      # 所有风扇锁 3000 rpm
sudo fanctl set 1 2600    # 或者只调 1 号风扇
sudo fanctl auto          # 风扇交回 macOS
sudo fanctl start         # 恢复按曲线控制
```

## 配置

`/usr/local/etc/fancontrol.conf`：

```
interval 3            # fanctl 接管风扇时，每隔几秒检查一次
auto_below 55         # PMU 温度低于它就交给 macOS（风扇可以停转）
auto_hysteresis 5     # 低于 55 - 5 = 50 °C 才交回，免得来回切
confirm_up 5          # 温度升高要连续 5 次检查都成立才动（默认值）
confirm_down 20       # 温度下降要连续 20 次检查都成立才动（默认值）
cpu_guard 90 100      # CPU 过热保护，这就是默认值；"cpu_guard off" 关闭

curve_pct 50 1        # <°C> <该风扇量程的百分比：0 = 最低转速，100 = 最高>
curve_pct 60 13       # 中间线性插值，两头之外取端点值
curve_pct 70 35
```

- **这里的温度是 PMU 温度，不是 CPU 核心温度。** 14 寸 M3 Pro 上 PMU 空闲约 45 °C、日常负载
  50–60 °C，而这时 CPU 核心可能已经 85 °C 以上。见[工作原理](#工作原理)。
- `curve <°C> <rpm>` 用绝对转速，替代 `curve_pct`，两种只能选一种。绝对转速只适合一个机型，
  百分比在哪台 Mac 上都能用。
- `cpu_guard <开始> <满速>`：fanctl 接管风扇期间，平滑后的 CPU 温度一过 `<开始>`，风扇至少转到
  （温度 − 开始）/（满速 − 开始）的量程，到 `<满速>` 时 100%。没有它的话，PMU 温度涨得太慢，
  长时间满载时风扇会被压在低速。
- `auto_below` 要高于空闲温度，否则风扇永远不停（`lap` 是故意这样）。
- 改完运行 `sudo fanctl reload`。它会先检查配置，有错就拒绝，旧配置继续生效。

## 工作原理

### 不会一有波动就动

变化要连续几次检查都成立，fanctl 才会动，而且风扇是慢慢变的：

| | 怎么确认 | 然后 |
|---|---|---|
| 接管 | 连续 `confirm_up` 次检查都 ≥ `auto_below`（默认 5 次，约 12 秒） | 从风扇当前的转速开始 |
| 加速 | 连续 `confirm_up` 次检查都需要更快 | 每秒最多升量程的 2%（从最低到最高约 50 秒） |
| 减速 | 连续 `confirm_down` 次检查都需要更慢（默认 20 次，约 1 分钟） | 每秒最多降 0.5%（降回去约 3 分钟） |
| 交回 | 连续 `confirm_down` 次检查都低于 `auto_below - auto_hysteresis` | macOS 从当前转速接手 |

确认接管期间 macOS 照常散热。唯一的例外：平滑后的 CPU 温度到了 `cpu_guard` 的满速点，
风扇立刻满速。

### 温度

| 来源 | 传感器 | 特点（14 寸 M3 Pro） | 用途 |
|---|---|---|---|
| HID | `PMU tdie*` 的最大值（没有就用 `PMU tdev*`） | 慢而稳：5 个 P 核满载 25 秒只涨 3 °C | 曲线、`auto_below` |
| SMC | `TCMz`（P 核最高温）和 `Tg*`（GPU） | 快而尖：同样负载 2 秒内 73 → 85 °C，空闲时也在 65–80 °C 间跳 | `cpu_guard`，用 10 秒时间常数平滑 |

Apple Silicon 的 HID 里没有 CPU 核心传感器，Intel 时代的 SMC 键（比如 `TC0P`）也没了。SMC 键名
每代芯片都不一样，所以 fanctl 启动时按前缀找 CPU 和 GPU 的键，并且先验证 `TCMz` 确实等于
`Tp*` 各键的最大值才用它；否则就读全部 `Tp*`/`Te*`/`Tg*` 键。

### 风扇

| 键 | 含义 |
|---|---|
| `F<i>Ac` | 实际转速 |
| `F<i>Mn` / `F<i>Mx` | 转速范围 |
| `F<i>Tg` | 目标转速（`flt`） |
| `F<i>Md` | 模式（`ui8`）：**1 = 手动，0 = SMC** |

容易踩的坑：

1. 不先把 `F<i>Md` 写成 1，目标转速会被忽略，而且写入照样返回成功。交回风扇要写 `F<i>Md = 0`，
   光把目标写成 0 没有用。
2. `F<i>Md` 只有一个字节。按 float 写的话，1.0 的第一个字节是 0x00。
3. 新目标要 30–100 ms 才生效，回读太早会误判为失败。
4. 没有 root 时 SMC 也返回成功但什么都不做，所以每次写完都回读校验。
5. SMC 对频繁写入有节流。fanctl 只在目标变化超过 100 rpm 时才写，几个风扇一批写完，
   批与批之间至少隔 2 秒。
6. 如果系统把风扇收回去（`F<i>Md` 读到 0）或目标被别人改了，fanctl 下一次检查时会重新接管。

### 尽量少打扰系统

每读一次传感器就是一次进出内核，所以 fanctl 只读当前状态需要的东西：

- 风扇在 macOS 手里时只读 PMU 温度，而且离 `auto_below` 越远，下次检查就隔得越久
  （它最快约每秒涨 0.15 °C，fanctl 按 0.5 °C/s 算），最长 30 秒。
- fanctl 接管风扇时：每个 `interval` 读 `TCMz` 和风扇模式；PMU 温度每 2 次读一次（确认交回期间
  每次都读），GPU 传感器每 5 次，风扇目标值每 10 次。转速范围只在启动时读一次。
- 信号走 kqueue：进程每次检查只醒一次，来信号立刻醒，从不轮询。
- 日志只在有变化时写：启动、重载、模式切换、曲线变化超过 3%、过热保护介入或解除，
  失败一分钟最多一条。

60 秒实测，对比"每 3 秒把所有传感器都读一遍"：

| | 上下文切换 | 系统调用 | CPU 时间 |
|---|---|---|---|
| macOS 管风扇（`balanced` 空闲时） | 4,439 → 172 | 2,510 → 211 | 63 → 12 ms |
| fanctl 管风扇（`lap`） | 4,592 → 563 | 2,710 → 590 | 65 → 20 ms |

常驻内存约 3 MB。

### 其他细节

- 连续 3 次读不到温度，风扇交回 macOS。
- 不装信号处理函数（IOKit 不是 async-signal-safe）：TERM/INT/HUP 通过 kqueue 收，在主循环里处理。
  TERM 先把风扇交回再退出，HUP 重新加载配置。
- LaunchDaemon 的 `ProcessType` 用 `Standard`（utility QoS，优先级 20）。全核满载时 sleep 500 ms，
  `Background`（优先级 4）平均要晚 180–210 ms 才醒，`Standard` 晚 56 ms。
- 同一时间只能有一个 `fanctl run` 控制风扇（`/var/run/fanctl.lock`）。

## 注意

- fanctl 写的是真实硬件状态，需要 root；卸载会把风扇交回 macOS。
- 不要同时运行其他风扇控制软件，会互相抢风扇。
- 手写的曲线不会比系统自带的控制更聪明，后者能看到更多传感器、反应更快。fanctl 是为了偏好：
  更安静、更凉，或者稳定的气流。
- 不支持 Intel Mac；没有风扇的 Mac 上守护进程会直接退出。
- 测试机器：14 寸 MacBook Pro（M3 Pro，Mac15,6，2 个风扇，2317–6800 rpm），macOS 14.4。
