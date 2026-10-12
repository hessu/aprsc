// build-all builds the aprsc release packages in LXD build containers,
// several platforms in parallel, and uploads them to the APT and RPM
// repositories. See README.md.
package main

import (
	"bytes"
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"
)

// platform is one build target. A container called build-<name> builds it,
// with i686 and x86_64 in the name replaced by i386 and amd64.
type platform struct {
	name string
	// RPM repository directory for the packages, for RPM platforms
	dest string
}

var platforms = []platform{
	{name: "ubuntu-2604-amd64"},
	{name: "ubuntu-2404-amd64"},
	{name: "ubuntu-2204-amd64"},
	{name: "debian-110-i386"},
	{name: "debian-110-amd64"},
	{name: "debian-120-amd64"},
	{name: "debian-130-amd64"},
	{name: "fedora-43-x86_64", dest: "fedora/43/x86_64"},
	{name: "fedora-44-x86_64", dest: "fedora/44/x86_64"},
}

const (
	// temporary download directories, one per platform below this
	dirBuildDown = "./build-down"
	// final downloaded builds
	dirBuildOut = "./build-out"
	// full output of each platform's build
	dirBuildLogs = "./build-logs"
	// where to upload builds
	uploadHost = "aprsc-dist"
	// directory on upload host
	dirUploadTmp = "/data/www/aprsc-dist/tmp-upload"
	// APT repository root directory
	dirUploadRepo = "/data/www/aprsc-dist/html/aprsc/apt"
	// RPM repository root directory
	dirUploadRepoRPM = "/data/www/aprsc-dist/html/aprsc/rpm"

	sshWait = 120 * time.Second
)

var (
	verbose bool
	// serialises writes to stdout from the parallel builds
	stdoutMu sync.Mutex
)

func (p platform) isRPM() bool {
	return strings.Contains(p.name, "fedora") || strings.Contains(p.name, "centos")
}

func (p platform) vm() string {
	vm := "build-" + p.name
	vm = strings.ReplaceAll(vm, "i686", "i386")
	vm = strings.ReplaceAll(vm, "x86_64", "amd64")
	return vm
}

// arch returns the last component of the platform name, e.g. x86_64.
func (p platform) arch() string {
	return p.name[strings.LastIndex(p.name, "-")+1:]
}

// lineWriter writes complete lines to stdout with a prefix, so that the
// output of parallel builds does not get mixed within a line.
type lineWriter struct {
	prefix string
	buf    []byte
}

func (w *lineWriter) Write(p []byte) (int, error) {
	w.buf = append(w.buf, p...)
	for {
		i := bytes.IndexByte(w.buf, '\n')
		if i < 0 {
			break
		}
		stdoutMu.Lock()
		fmt.Printf("%s%s\n", w.prefix, w.buf[:i])
		stdoutMu.Unlock()
		w.buf = w.buf[i+1:]
	}
	return len(p), nil
}

func (w *lineWriter) flush() {
	if len(w.buf) > 0 {
		w.Write([]byte("\n"))
	}
}

// job is the build of one platform. Command output goes to the log file,
// and with -v to stdout too; progress messages go to both.
type job struct {
	plat    platform
	vm      string
	ctx     context.Context
	logPath string
	log     *os.File
	out     io.Writer
	console *lineWriter
}

func (j *job) printf(format string, args ...interface{}) {
	msg := fmt.Sprintf(format, args...)
	fmt.Fprintf(j.log, "=== %s\n", msg)
	stdoutMu.Lock()
	fmt.Printf("%s%s\n", j.console.prefix, msg)
	stdoutMu.Unlock()
}

// run runs a command, logging its output.
func (j *job) run(name string, args ...string) error {
	fmt.Fprintf(j.log, "$ %s %s\n", name, strings.Join(args, " "))
	cmd := exec.CommandContext(j.ctx, name, args...)
	cmd.Stdout = j.out
	cmd.Stderr = j.out
	err := cmd.Run()
	if verbose {
		j.console.flush()
	}
	return err
}

// step runs a command, and wraps a failure with the step's name.
func (j *job) step(what, name string, args ...string) error {
	j.printf("%s ...", what)
	if err := j.run(name, args...); err != nil {
		return fmt.Errorf("vm %s %s failed: %w", j.vm, what, err)
	}
	return nil
}

// vmState returns the container's status as lxc reports it, e.g.
// "Running" or "Stopped".
func vmState(ctx context.Context, vm string) (string, error) {
	out, err := exec.CommandContext(ctx, "lxc", "info", vm).Output()
	if err != nil {
		return "", fmt.Errorf("lxc info %s: %w", vm, err)
	}
	m := regexp.MustCompile(`(?m)^Status:\s+(\S+)`).FindSubmatch(out)
	if m == nil {
		return "", fmt.Errorf("lxc info %s: no status in output", vm)
	}
	return string(m[1]), nil
}

func (j *job) vmUp() error {
	state, err := vmState(j.ctx, j.vm)
	if err != nil {
		return err
	}
	switch state {
	case "Running":
		j.printf("vm %s already running", j.vm)
		return nil
	case "Stopped":
		return j.step("start", "lxc", "start", j.vm)
	}
	return fmt.Errorf("vm_up %s: unknown state %s", j.vm, state)
}

func (j *job) vmShutdown() error {
	state, err := vmState(j.ctx, j.vm)
	if err != nil {
		return err
	}
	switch state {
	case "Running":
		return j.step("stop", "lxc", "stop", j.vm)
	case "Stopped":
		return nil
	}
	return fmt.Errorf("vm_down %s: unknown state %s", j.vm, state)
}

// tcpWait waits until the address accepts three connections, so that a
// freshly started sshd is really up.
func (j *job) tcpWait(hostport string, timeout time.Duration) error {
	j.printf("waiting for %s ...", hostport)
	ok := 0
	end := time.Now().Add(timeout)
	var lastErr error
	for time.Now().Before(end) {
		c, err := net.DialTimeout("tcp", hostport, 2*time.Second)
		if err == nil {
			c.Close()
			ok++
			if ok >= 3 {
				return nil
			}
		} else {
			lastErr = err
			fmt.Fprintf(j.log, " ... %s: %v\n", hostport, err)
		}
		select {
		case <-j.ctx.Done():
			return j.ctx.Err()
		case <-time.After(300 * time.Millisecond):
		}
	}
	if ok > 0 {
		return nil
	}
	return fmt.Errorf("vm %s ssh is not accepting connections: %v", j.vm, lastErr)
}

// download fetches the build products into a fresh download directory of
// this platform and returns their names that match the pattern.
func (j *job) download(srcs []string, match *regexp.Regexp) (string, []string, error) {
	down := filepath.Join(dirBuildDown, j.plat.name)
	if err := os.RemoveAll(down); err != nil {
		return "", nil, err
	}
	if err := os.MkdirAll(down, 0755); err != nil {
		return "", nil, err
	}
	args := append(srcs, down+"/")
	if err := j.step("download packages", "scp", args...); err != nil {
		return "", nil, fmt.Errorf("vm %s build product download failed: %w", j.vm, err)
	}
	ents, err := os.ReadDir(down)
	if err != nil {
		return "", nil, err
	}
	var products []string
	for _, e := range ents {
		if e.Type().IsRegular() && match.MatchString(e.Name()) {
			products = append(products, e.Name())
		}
	}
	return down, products, nil
}

func (j *job) buildDeb(tgz string) error {
	dTgz := filepath.Base(tgz)
	dir := regexp.MustCompile(`\.tar.*`).ReplaceAllString(dTgz, "")

	if err := j.step("cleanup", "ssh", j.vm, "rm -rf buildtmp && mkdir -p buildtmp"); err != nil {
		return err
	}
	if err := j.step("upload sources", "scp", tgz, j.vm+":buildtmp/"); err != nil {
		return err
	}
	if err := j.step("extract sources", "ssh", j.vm, "cd buildtmp && tar xfz "+dTgz); err != nil {
		return err
	}
	if err := j.step("build", "ssh", j.vm, "cd buildtmp/"+dir+"/src && ./configure && DEB_BUILD_HARDENING=1 make make-deb"); err != nil {
		return err
	}
	down, products, err := j.download([]string{
		j.vm + ":buildtmp/" + dir + "/*.deb",
		j.vm + ":buildtmp/" + dir + "/*.changes",
	}, regexp.MustCompile(`^aprsc.*\.(changes|deb)`))
	if err != nil {
		return err
	}
	for _, f := range products {
		if err := os.Rename(filepath.Join(down, f), filepath.Join(dirBuildOut, f)); err != nil {
			return err
		}
		j.printf("got %s", f)
	}
	return nil
}

func (j *job) buildRPM(tgz string) error {
	dTgz := filepath.Base(tgz)

	if err := j.step("cleanup", "ssh", j.vm, "rm -rf buildtmp && mkdir -p buildtmp && rm -rf rpmbuild/*"); err != nil {
		return err
	}
	if err := j.step("upload sources", "scp", tgz, j.vm+":buildtmp/"); err != nil {
		return err
	}
	if err := j.step("build RPM", "ssh", j.vm, "cd buildtmp && rpmbuild --target "+j.plat.arch()+" -ta "+dTgz); err != nil {
		return err
	}
	// e.g. aprsc-1.0.4.g1447ccdM-1.x86_64.rpm, not the debuginfo packages
	down, products, err := j.download([]string{j.vm + ":rpmbuild/RPMS/*/aprsc-*.rpm"},
		regexp.MustCompile(`^aprsc-\d.*\.rpm`))
	if err != nil {
		return err
	}
	for _, f := range products {
		of := filepath.Join(dirBuildOut, j.plat.name+"___"+f)
		if err := os.Rename(filepath.Join(down, f), of); err != nil {
			return err
		}
		j.printf("got %s", filepath.Base(of))
	}
	return nil
}

// build builds one platform. The container is left running if the
// build fails, for a look inside.
func build(ctx context.Context, plat platform, tgz string) error {
	j := &job{
		plat:    plat,
		vm:      plat.vm(),
		ctx:     ctx,
		logPath: filepath.Join(dirBuildLogs, plat.name+".log"),
		console: &lineWriter{prefix: fmt.Sprintf("[%s] ", plat.name)},
	}
	var err error
	if j.log, err = os.Create(j.logPath); err != nil {
		return err
	}
	defer j.log.Close()
	j.out = j.log
	if verbose {
		j.out = io.MultiWriter(j.log, j.console)
	}

	j.printf("building on %s, log in %s", j.vm, j.logPath)
	if err := j.vmUp(); err != nil {
		return err
	}
	// sshd may still be starting even if the container was running
	if err := j.tcpWait(net.JoinHostPort(j.vm, "22"), sshWait); err != nil {
		return err
	}
	if plat.isRPM() {
		err = j.buildRPM(tgz)
	} else {
		err = j.buildDeb(tgz)
	}
	if err != nil {
		return err
	}
	return j.vmShutdown()
}

// logTail returns the last n lines of a file.
func logTail(path string, n int) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	lines := strings.Split(strings.TrimRight(string(b), "\n"), "\n")
	if len(lines) > n {
		lines = lines[len(lines)-n:]
	}
	return strings.Join(lines, "\n")
}

func buildAll(ctx context.Context, plats []platform, tgz string, parallel int) error {
	for _, d := range []string{dirBuildOut, dirBuildLogs} {
		if err := os.MkdirAll(d, 0755); err != nil {
			return err
		}
	}

	type result struct {
		plat platform
		err  error
		dur  time.Duration
	}
	results := make([]result, len(plats))
	// the builds start in the listed order
	queue := make(chan int, len(plats))
	for i := range plats {
		queue <- i
	}
	close(queue)
	var wg sync.WaitGroup
	for w := 0; w < parallel; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range queue {
				p := plats[i]
				start := time.Now()
				err := ctx.Err()
				if err == nil {
					err = build(ctx, p, tgz)
				}
				results[i] = result{plat: p, err: err, dur: time.Since(start)}
				stdoutMu.Lock()
				if err != nil {
					fmt.Printf("[%s] FAILED: %v\n", p.name, err)
				} else {
					fmt.Printf("[%s] done in %s\n", p.name, results[i].dur.Round(time.Second))
				}
				stdoutMu.Unlock()
			}
		}()
	}
	wg.Wait()

	failed := 0
	fmt.Println()
	fmt.Println("Summary:")
	for _, r := range results {
		if r.err != nil {
			failed++
			fmt.Printf("  %-20s FAILED %s: %v\n", r.plat.name, r.dur.Round(time.Second), r.err)
		} else {
			fmt.Printf("  %-20s ok     %s\n", r.plat.name, r.dur.Round(time.Second))
		}
	}
	for _, r := range results {
		if r.err != nil && !errors.Is(r.err, context.Canceled) {
			logPath := filepath.Join(dirBuildLogs, r.plat.name+".log")
			fmt.Printf("\n--- %s: last lines of %s:\n%s\n", r.plat.name, logPath, logTail(logPath, 30))
		}
	}
	if failed > 0 {
		return fmt.Errorf("%d of %d builds failed", failed, len(results))
	}
	return nil
}

// runInteractive runs a command attached to the terminal, for ssh -t and
// the gpg passphrase prompt.
func runInteractive(name string, args ...string) error {
	cmd := exec.Command(name, args...)
	cmd.Stdin = os.Stdin
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}

var debDistRe = regexp.MustCompile(`.*\+([[:alpha:]]+)`)

// debDist returns the distribution codename in a .changes file name, e.g.
// bookworm in aprsc_2.1.20.g7ddd088+bookworm1_amd64.changes
func debDist(name string) (string, error) {
	m := debDistRe.FindStringSubmatch(name)
	if m == nil {
		return "", fmt.Errorf("no distribution codename in %s", name)
	}
	return m[1], nil
}

func upload() error {
	ents, err := os.ReadDir(dirBuildOut)
	if err != nil {
		return err
	}
	var all, debChanges, rpms []string
	for _, e := range ents {
		all = append(all, e.Name())
		if !e.Type().IsRegular() {
			continue
		}
		if strings.Contains(e.Name(), ".changes") {
			debChanges = append(debChanges, e.Name())
		}
		if strings.Contains(e.Name(), ".rpm") {
			rpms = append(rpms, e.Name())
		}
	}
	fmt.Printf("products: %s\n", strings.Join(all, " "))
	if len(all) == 0 {
		return fmt.Errorf("nothing to upload in %s", dirBuildOut)
	}

	if err := runInteractive("ssh", uploadHost, "rm -rf "+dirUploadTmp+" && mkdir -p "+dirUploadTmp); err != nil {
		return fmt.Errorf("upload host upload_tmp cleanup failed: %w", err)
	}
	args := []string{"-r"}
	for _, f := range all {
		args = append(args, filepath.Join(dirBuildOut, f))
	}
	args = append(args, uploadHost+":"+dirUploadTmp+"/")
	if err := runInteractive("scp", args...); err != nil {
		return fmt.Errorf("upload to upload_host failed: %w", err)
	}

	if len(debChanges) > 0 {
		cmd := "cd " + dirUploadRepo + " && eval `gpg-agent --daemon`;"
		for _, c := range debChanges {
			dist, err := debDist(c)
			if err != nil {
				return err
			}
			path := dirUploadTmp + "/" + c
			cmd += fmt.Sprintf(" echo 'dist %s: %s'; reprepro --ask-passphrase -Vb . include '%s' '%s';",
				dist, path, dist, path)
		}
		cmd += " killall gpg-agent"
		if err := runInteractive("ssh", "-t", uploadHost, cmd); err != nil {
			return fmt.Errorf("APT repository update failed: %w", err)
		}
	}

	if len(rpms) > 0 {
		dests := map[string]string{}
		for _, p := range platforms {
			if p.dest != "" {
				dests[p.name] = p.dest
			}
		}
		rpmdirs := map[string]bool{}
		for _, rpm := range rpms {
			dist, rpmname, ok := strings.Cut(rpm, "___")
			dest := dests[dist]
			if !ok || dest == "" {
				return fmt.Errorf("RPM upload: No such distribution upload configured: %s", dist)
			}
			fmt.Printf("%s: moving to %s/%s\n", dist, dest, rpmname)
			cmd := "cd " + dirUploadRepoRPM + " && mkdir -p " + dest + " && mv " + dirUploadTmp + "/" + rpm + " " + dest + "/" + rpmname
			if err := runInteractive("ssh", "-t", uploadHost, cmd); err != nil {
				return fmt.Errorf("RPM move failed: %w", err)
			}
			rpmdirs[dirUploadRepoRPM+"/"+dest] = true
		}
		var dirs []string
		for d := range rpmdirs {
			dirs = append(dirs, d)
		}
		sort.Strings(dirs)
		cmd := "for i in " + strings.Join(dirs, " ") + "; do createrepo_c $i; done"
		if err := runInteractive("ssh", "-t", uploadHost, cmd); err != nil {
			return fmt.Errorf("YUM repository creation failed: %w", err)
		}
	}
	return nil
}

func main() {
	mode := flag.String("m", "build", "mode: build or upload")
	only := flag.String("o", "", "build only the platforms matching this regexp")
	parallel := flag.Int("j", 3, "number of containers building at a time")
	list := flag.Bool("l", false, "list the platforms and exit")
	flag.BoolVar(&verbose, "v", false, "show the build output, not just the steps")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: build-all [-v] [-j <parallel>] [-o <buildonly>] [-m build] <source.tar.gz>\n"+
			"       build-all -m upload\n"+
			"       build-all -l\n\n")
		flag.PrintDefaults()
	}
	flag.Parse()

	if *list {
		for _, p := range platforms {
			fmt.Printf("%-20s %s\n", p.name, p.vm())
		}
		return
	}

	switch *mode {
	case "build":
		if flag.NArg() != 1 {
			flag.Usage()
			os.Exit(1)
		}
		tgz := flag.Arg(0)
		if st, err := os.Stat(tgz); err != nil || !st.Mode().IsRegular() {
			fmt.Fprintf(os.Stderr, "No such source package: %s\n", tgz)
			os.Exit(1)
		}
		if *parallel < 1 {
			fmt.Fprintf(os.Stderr, "-j must be at least 1\n")
			os.Exit(1)
		}
		plats := platforms
		if *only != "" {
			re, err := regexp.Compile(*only)
			if err != nil {
				fmt.Fprintf(os.Stderr, "Bad -o regexp: %v\n", err)
				os.Exit(1)
			}
			plats = nil
			for _, p := range platforms {
				if re.MatchString(p.name) {
					plats = append(plats, p)
				}
			}
			if len(plats) == 0 {
				fmt.Fprintf(os.Stderr, "No platforms match %q\n", *only)
				os.Exit(1)
			}
		}
		// ^C kills the running commands; containers are left as they are
		ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
		defer stop()
		if err := buildAll(ctx, plats, tgz, *parallel); err != nil {
			fmt.Fprintf(os.Stderr, "\n%v\n", err)
			os.Exit(1)
		}
	case "upload":
		if err := upload(); err != nil {
			fmt.Fprintf(os.Stderr, "%v\n", err)
			os.Exit(1)
		}
	default:
		fmt.Fprintf(os.Stderr, "Unknown mode %q\n", *mode)
		flag.Usage()
		os.Exit(1)
	}
}
