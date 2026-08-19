# Homebrew cask for macOS. Installs Graph.app into /Applications — so it is
# in Spotlight, Launchpad and the Dock — and links the graph command it
# carries into PATH. Filled in from the release tarball the workflow builds.
cask "graph" do
  version "__VERSION__"
  sha256 "__SHA__"

  url "https://github.com/wrk-labs/graph/releases/download/v#{version}/graph-#{version}-darwin-arm64.tar.gz"
  name "graph"
  desc "Filesystem repository shared over SMB"
  homepage "https://wrklabs.org/graph"

  depends_on macos: ">= :monterey"
  depends_on arch: :arm64

  app "Graph.app"
  binary "#{appdir}/Graph.app/Contents/MacOS/graph"

  # The app is built by CI, not yet signed with a Developer ID. Clearing the
  # quarantine flag lets it open like a locally built one would; drop this
  # once releases are notarized.
  postflight do
    system_command "/usr/bin/xattr",
                   args: ["-cr", "#{appdir}/Graph.app"],
                   sudo: false
  end

  zap trash: "~/Library/Application Support/graph"
end
