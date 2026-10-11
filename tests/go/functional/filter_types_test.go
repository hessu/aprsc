package functional

import (
	"strings"
	"testing"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// connectFilterPair connects the transmitter N5CAL-1 on the igate port and the
// receiver N5CAL-2 on the client-only port, with no filter on the receiver.
func connectFilterPair(t *testing.T) (tx, rx *isclient.Plain) {
	t.Helper()
	tx = isclient.Connect(t, "localhost:55580", "N5CAL-1")
	rx = isclient.Connect(t, "localhost:55581", "N5CAL-2")
	return tx, rx
}

// Prefix, buddy and object filters. Port of t/33filter-pref-buddy-obj.t.
func TestFilterPrefixBuddyObject(t *testing.T) {
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	tx, rx := connectFilterPair(t)

	const pos = ":!6028.51N/02505.68E#"

	t.Run("prefix filter", func(t *testing.T) {
		rx.Command("#filter p/OH/G")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			passSame("OH prefix passes", "OH0TES>APRS,OH2RDG*,WIDE,qAR,"+login+pos+" should pass prefix filter"),
			passSame("G prefix passes", "G0TES>APRS,OH2RDG*,WIDE,qAR,"+login+pos+" should pass prefix filter"),
			drop("N prefix dropped",
				"N0TES>APRS,OH2RDG*,WIDE,qAR,"+login+pos+" should drop prefix filter",
				"G0TES-2>APRS,OH2RDG*,WIDE,qAR,"+login+pos+" should pass prefix filter"),
		})
	})

	t.Run("buddy filter", func(t *testing.T) {
		rx.Command("#filter b/OH0TES/OH2TES b/OH7*")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			passSame("buddy matches", "OH0TES>APRS,OH2RDG*,WIDE,qAR,"+login+pos+" should pass buddy filter"),
			// the previously set prefix filter should no longer pass
			drop("previous prefix filter no longer passes",
				"G0TES>APRS,OH2RDG*,WIDE,qAR,"+login+pos+" should drop buddy filter",
				"OH0TES>APRS,OH2RDG*,WIDE,qAR,"+login+pos+" helper pass"),
			// the buddy filter does not act like a prefix filter
			drop("buddy is not a prefix",
				"OH0TES-9>APRS,OH2RDG*,WIDE,qAR,"+login+pos+" should drop buddy filter",
				"OH0TES>APRS,OH2RDG*,WIDE,qAR,"+login+pos+" helper pass2"),
			passSame("wildcard in end", "OH7TES>APRS,OH2RDG*,WIDE,qAR,"+login+pos+" should pass wildcard buddy filter"),
			// A wildcard in the middle is not supported in any of the products.
		})
	})

	t.Run("object filter", func(t *testing.T) {
		rx.Command("#filter o/OBJ1/OBJ2/ISS/PRE*")

		var steps []isclient.Step
		if strings.HasPrefix(harness.ProductName(), "javap") {
			t.Log("javAPRSSrvr o/ filter requires spaces, skipping the object cases")
		} else {
			steps = append(steps,
				// the previously set buddy filter should no longer pass
				drop("previous buddy filter no longer passes",
					"G0TES>APRS,OH2RDG*,WIDE,qAR,"+login+pos+" should drop buddy filter",
					"HSRC>APRS,qAR,"+login+":;OBJ1     *090902z6010.78N/02451.11E-Object 1"),
				passSame("object name matches",
					"SRC>APRS,qAR,"+login+":;OBJ2     *090902z6010.78N/02451.11E-Object 2"),
				// another object, this one having a HHMMSSh timestamp
				passSame("object with HHMMSSh timestamp",
					"KJ4ERJ-15>APZTLE,TCPIP*,qAC,FOURTH:;ISS      *060312h4541.51N\\14357.24ESMsg4Pass }k1mlNcqQzAq5a0N5;A+Q#c+lN{!w+:!"),
			)
		}

		steps = append(steps,
			passSame("wildcard in end",
				"SRC>APRS,qAR,"+login+":;PREFIX   *090902z6010.78N/02451.11E-Object prefix"),
			// Wildcards in the beginning and in the middle are not supported.
			// Items should match the object filter too.
			passSame("item 1", "SRC>APRS,qAR,"+login+":)OBJ1!4903.50N/07201.75WA"),
			passSame("item 2", "SRC>APRS,qAR,"+login+":)OBJ2!4903.50N/07201.75WA"),
		)

		isclient.RunSteps(t, tx, rx, steps)
	})

	s.Stop()
}

// Type and symbol filters. Port of t/34filter-type-symbol.t.
func TestFilterTypeSymbol(t *testing.T) {
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	tx, rx := connectFilterPair(t)

	const wx = "@100857z5241.73N/00611.14E_086/002g008t064r000p000P000h63b10102L810.DsIP-VP"
	const pos = ":!6028.51N/02505.68E#"
	const obj = ":;OBJ1     *090902z6010.78N/02451.11E-Object 1"
	const telemetry = ":T#931,113,000,000,000,000,00000000"
	msg := func(to, text string) string {
		return "::" + to + strings.Repeat(" ", 9-len(to)) + ":" + text
	}

	t.Run("first half of the types", func(t *testing.T) {
		rx.Command("#filter t/poimq -t/nw")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			drop("status dropped, position passes",
				"ST1>APRS,qAR,"+login+":>status type drop",
				"ST2>APRS,qAR,"+login+":!6028.51N/02505.68E# type pos"),
			drop("telemetry dropped, object passes",
				"ST3>APRS,qAR,"+login+telemetry,
				"ST4>APRS,qAR,"+login+obj),
			drop("user-defined dropped, item passes",
				"ST5>APRS,qAR,"+login+":{Q1qwerty",
				"ST6>APRS,qAR,"+login+":)OBJ1!4903.50N/07201.75WA"),
			// A NWS object message would be a better drop test, but there is
			// no example of it.
			passSame("message passes", "ST7>APRS,qAR,"+login+msg("DSTC", "normal message{ac")),
			// wx must be explicitly dropped with -t/w since the wx packet also
			// has a position and would match t/p.
			drop("wx dropped, query message passes",
				"ST10>APRS,qAR,"+login+":"+wx,
				"ST9>APRS,qAR,"+login+msg("IGCALL", "?APRSD")),
		})
	})

	t.Run("second half of the types", func(t *testing.T) {
		rx.Command("#filter t/stunwc")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			drop("position dropped, status passes",
				"ST12>APRS,qAR,"+login+pos+" pos drop",
				"ST11>APRS,qAR,"+login+":>status pass"),
			drop("object dropped, telemetry passes",
				"ST14>APRS,qAR,"+login+obj,
				"ST13>APRS,qAR,"+login+telemetry),
			drop("item dropped, user-defined passes",
				"ST16>APRS,qAR,"+login+":)OBJ1!4903.50N/07201.75WA",
				"ST15>APRS,qAR,"+login+":{Q1qwerty"),
			drop("message dropped, weather passes",
				"ST17>APRS,qAR,"+login+msg("DSTC", "normal message again{ff"),
				"ST18>APRS,qAR,"+login+":"+wx),
			drop("CWOP passes, position dropped",
				"XW1AA>APRS,qAR,"+login+pos+" pos drop",
				"CW1234>APRS,qAR,"+login+pos+" CWOP pass"),
			// CWOP with overlay on symbol
			drop("CWOP with overlay passes, position dropped",
				"XW1AZ>APRS,qAR,"+login+pos+" pos drop again",
				"YACHTS>APRS,qAR,"+login+":@055503z4420.57N112405.53W_287/007g020t055r007p053P019h87b10115.U21h"),
			drop("EW CWOP passes, position dropped",
				"FW1AB>APRS,qAR,"+login+pos+" pos drop",
				"EW1234>APRS,qAR,"+login+pos+" CWOP pass"),
		})
	})

	t.Run("distance in type filter", func(t *testing.T) {
		rx.Command("#filter t/o/OH2TI/10")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			// OH2TI's position/wx packet must not pass, since it is not an
			// object. It is sent before the drop: if it were forwarded it
			// would show up instead of the helper. OH2KXH's position is just
			// under 10 km from OH2TI, but it's not an object. The OH2V object
			// is less than 10 km away.
			{
				Name:   "position and wx dropped, nearby object passes",
				Before: "OH2TI>APRX1M,qAR," + login + ":/010848h6011.24N/02450.18E_c237s005g009t048h88b10042",
				Send:   "OH2KXH>APX200,qAR," + login + ":=6014.22N/02443.08E_193/000g000t047r000P000p000h93b10080XRSW",
				Helper: "OH1UK>APRS,qAR," + login + ":;OH2V     *100802z6013.32N/02445.49EyNokia Espoo Club",
			},
			// OH2FRM DW3161 is some 12 km from OH2TI, OH1UK is 9 km from OH2TI
			drop("far object dropped, near object passes",
				"OH2FRM>APRS,qAR,"+login+":;OH2FRM   *092253z6017.00N/02457.34E-Tom's",
				"OH1UK>APRS,qAR,"+login+":;OH2WW    *100802z6008.09N/02443.65EYPatrick's"),
		})
	})

	t.Run("symbol filter", func(t *testing.T) {
		// passes all House and Car symbols (primary table)
		rx.Command("#filter s/->")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			drop("digi dropped, house passes",
				"OH2RDP>BEACON,qAR,"+login+":!6028.51N/02505.68E#PHG7220/drop digi sym",
				"OH2FIH>APX201,qAR,"+login+":=/0&ynTc8A-  pass car sym"),
			drop("digi dropped, car passes",
				"OH3RDP>BEACON,qAR,"+login+":!6028.51N/02505.68E#PHG7220/drop digi sym again",
				"OH2FIH-9>APRS,qAR,"+login+":!6018.16N/02421.26E>"),
		})

		// passes all Digi with or without overlay
		rx.Command("#filter s//#")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			drop("car dropped, digi passes",
				"OH5TES>APRS,qAR,"+login+":!6018.16N/02421.26E> drop car",
				"OH2RDP>BEACON,qAR,"+login+":!6028.51N\\02505.68E# digi symbol pass"),
			drop("car dropped, digi with optional overlay passes",
				"OH5TES-2>APRS,qAR,"+login+":!6018.16N/02421.26E> drop car again",
				"OH2RDS>APRS,qAR,"+login+":!6016.66NN02515.26E# digi symbol opt overlay"),
		})

		// passes all Digi with overlay of capital T
		rx.Command("#filter s//#/T")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			drop("digi with other overlay dropped, mandatory overlay passes",
				"OH2RDS-2>APRS,qAR,"+login+":!6016.66NN02515.26E# digi sym mand overlay drop",
				"OH2RDS-1>APRS,qAR,"+login+":!6016.66NT02515.26E# digi sym mand overlay pass"),
		})
	})

	s.Stop()
}

// Filters for digipeater, entrycall and unproto. Port of
// t/35filter-digi-entry-unproto.t.
func TestFilterDigiEntryUnproto(t *testing.T) {
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	const pos = ":!6016.66NT02515.26E# "
	tx, rx := connectFilterPair(t)

	t.Run("digipeater", func(t *testing.T) {
		rx.Command("#filter d/OH7RDA/OH2RDS/OH9*")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			// The filter must only match when the path element is USED*
			drop("used digi passes, unused dropped",
				"SRC2>APRS,OH7RDA,qAR,"+login+pos+"pass digi1 unused",
				"SRC1>APRS,OH7RDA*,qAR,"+login+pos+"pass digi1"),
			// The * in a later digi also marks the previous ones as used.
			drop("star in later digi marks previous used",
				"SRC4>APRS,OH7RDA,OH8RDA,qAR,"+login+pos+"pass digi2 unused",
				"SRC3>APRS,OH7RDA,OH8RDA*,qAR,"+login+pos+"pass digi2"),
			// the same with a wildcarded filter entry
			drop("wildcard, used digi passes",
				"SRC6>APRS,OH9RDA,qAR,"+login+pos+"pass digi1 unused",
				"SRC5>APRS,OH9RDA*,qAR,"+login+pos+"pass digi1"),
			drop("wildcard, star in later digi",
				"SRC8>APRS,OH9RDA,OH8RDA,qAR,"+login+pos+"pass digi2 unused",
				"SRC7>APRS,OH9RDA,OH8RDA*,qAR,"+login+pos+"pass digi2"),
		})
	})

	t.Run("entrycall", func(t *testing.T) {
		rx.Command("#filter e/" + login + "/IGA*")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			drop("entrycall",
				"ESRC2>APRS,qAR,BAA"+pos+"drop entrycall",
				"ESRC1>APRS,qAR,"+login+pos+"pass entrycall"),
			// with a wildcard, and the matching call as non-first entry
			drop("entrycall wildcard",
				"ESRC4>APRS,qAR,FOO,IGATE2"+pos+"drop entrycall wildcard",
				"ESRC3>APRS,qAR,IGATE1"+pos+"pass entrycall wildcard"),
		})
	})

	t.Run("unproto", func(t *testing.T) {
		rx.Command("#filter u/ALTNET/APZ*")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			drop("unproto",
				"USRC2>ALTBLA,qAR,"+login+pos+"drop unproto",
				"USRC1>ALTNET,qAR,"+login+pos+"pass unproto"),
			drop("unproto wildcard",
				"USRC4>APXMDR,qAR,"+login+pos+"drop unproto wildcard",
				"USRC3>APZMDR,qAR,"+login+pos+"pass unproto wildcard"),
		})
	})

	s.Stop()
}

// Q construct filters. Port of t/37filter-qconstr.t.
func TestFilterQConstr(t *testing.T) {
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	const pos = ":!2334.10"
	tx, rx := connectFilterPair(t)

	rx.Command("#filter q/RUo")
	isclient.RunSteps(t, tx, rx, []isclient.Step{
		drop("qAR passes, qAC dropped",
			login+">APN390"+pos+"N/04719.70W# drop qAC",
			"PU2TFG-15>APN390,qAR,"+login+pos+"S/04719.70W# pass qAR"),
		// this drop is outside documentation, but it is dropped
		drop("qAU passes, qAU from own login dropped",
			login+">APN390,qAU,"+login+pos+"N/04719.70W# drop qAU)",
			"PU2TFG-1>APN390,qAU,"+login+pos+"S/04719.70W# pass qAU"),
		drop("qAo passes, qAr dropped",
			"DR0P>APN39X,qAr,CL1ENT"+pos+"N/04719.70W# drop qAr)",
			"PU2TFG-2>APN390,qAo,SRVR1"+pos+"S/04719.70W# pass qAo"),
		drop("qAo passes, qAO dropped",
			"DR0P-2>APN39,qAO,CL1ENT"+pos+"N/04719.70W# drop qAr)",
			"PU2TFG-2>APN390,qAo,SRVR1"+pos+"S/04719.70W# pass qAo"),
	})

	s.Stop()
}
