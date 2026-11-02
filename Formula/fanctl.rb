class Fanctl < Formula
  desc "Temperature-based fan control for Apple Silicon Macs"
  homepage "https://github.com/itswl/fanctl"
  url "https://github.com/itswl/fanctl/archive/refs/tags/v1.0.4.tar.gz"
  sha256 "da4c3cb705addc90a452ec80e1edfef33cfb4132438e995af6a9c74d682bcbdd"

  depends_on arch: :arm64
  depends_on macos: :sonoma

  def install
    system ENV.cc, "-O2", "-Wall", "-o", "fanctl", "fanctl.c",
           "-framework", "IOKit", "-framework", "CoreFoundation",
           "-DCONF_PATH=\"#{etc}/fancontrol.conf\"",
           "-DPRESET_DIR=\"#{opt_pkgshare}/presets\"",
           "-DLOG_PATH=\"#{var}/log/fancontrol.log\"",
           "-DLABEL=\"#{plist_name}\"",
           "-DSETUP_HINT=\"set it up with: sudo brew services start fanctl\""
    bin.install "fanctl"
    cp "presets/balanced.conf", "fancontrol.conf"
    etc.install "fancontrol.conf"
    pkgshare.install "presets"
  end

  def caveats
    <<~EOS
      fanctl drives the fans from a root service. Start it, also at every boot, with:
        sudo brew services start fanctl
      Use either Homebrew or the one-line installer from the README, not both.
      brew services makes the files root-owned; before brew upgrade/uninstall run:
        sudo chown -R "$(whoami)" "$(brew --cellar fanctl)"
    EOS
  end

  service do
    run [opt_bin/"fanctl", "run", "--config", etc/"fancontrol.conf"]
    require_root true
    keep_alive true
    log_path var/"log/fancontrol.log"
    error_log_path var/"log/fancontrol.log"
  end

  test do
    assert_match "fanctl #{version}", shell_output("#{bin}/fanctl version")
    assert_match "ok", shell_output("#{bin}/fanctl check --config #{pkgshare}/presets/balanced.conf")
  end
end
