// Command aprs-is is a set of APRS-IS client tools for testing and loading a
// server by hand: receiving, transmitting, copying and replaying traffic, and
// simulating many clients at once. It replaces the Perl scripts which were in
// tools/.
package main

import (
	"fmt"
	"os"
	"sort"
)

type subcommand struct {
	run   func(args []string) error
	usage string
}

var subcommands = map[string]subcommand{
	"rx":      {runRx, "receive packets and print them"},
	"is2rx":   {runIS2Rx, "receive packets over IS2 and print them with their metadata"},
	"xmit":    {runXmit, "send a set of sample packets slowly, printing what comes back"},
	"copy":    {runCopy, "copy the feed of one server into another"},
	"feed":    {runFeed, "send packets read from standard input"},
	"multirx": {runMultiRx, "keep many receiving connections open"},
	"coresim": {runCoreSim, "simulate the client load of the 2008 APRS-IS core servers"},
	"post":    {runPost, "send packets with an HTTP POST"},
}

func usage() {
	fmt.Fprintf(os.Stderr, "Usage: aprs-is <command> [options] [arguments]\n\nCommands:\n")
	names := make([]string, 0, len(subcommands))
	for n := range subcommands {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		fmt.Fprintf(os.Stderr, "  %-8s %s\n", n, subcommands[n].usage)
	}
	fmt.Fprintf(os.Stderr, "\n'aprs-is <command> -h' shows the options of a command.\n")
}

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}
	sc, ok := subcommands[os.Args[1]]
	if !ok {
		if os.Args[1] != "-h" && os.Args[1] != "help" {
			fmt.Fprintf(os.Stderr, "aprs-is: unknown command %q\n\n", os.Args[1])
		}
		usage()
		os.Exit(2)
	}
	if err := sc.run(os.Args[2:]); err != nil {
		fmt.Fprintf(os.Stderr, "aprs-is %s: %v\n", os.Args[1], err)
		os.Exit(1)
	}
}
