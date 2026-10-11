package functional

import (
	"fmt"
	"testing"
	"time"

	"github.com/hessu/aprsc/tests/go/internal/harness"
	"github.com/hessu/aprsc/tests/go/internal/isclient"
)

// Filters for area (a/), my position range (m/) and friend position range
// (f/). Port of t/36filter-area-my-friend.t.
func TestFilterAreaMyFriend(t *testing.T) {
	s := harness.Start(t, "basic")

	const login = "N5CAL-1"
	const rxLogin = "N5CAL-2"

	tx := isclient.Connect(t, "localhost:55580", login)
	rx := isclient.Connect(t, "localhost:55581", rxLogin, isclient.WithPasscode("-1"))

	t.Run("area in north-east", func(t *testing.T) {
		rx.SetFilter("a/60.1874/24.8362/60.1872/24.8365")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			drop("position outside",
				"DR0P>APRX1M,qAR,"+login+":/054048h6011.30N/02450.18E_c212s003g006t053h82b10046",
				login+">APRX1M,qAR,"+login+":/054048h6011.24N/02450.18E_c212s003g006t053h82b10046"),
			drop("position outside, telemetry without position passes",
				"DR0P>APRX1M,qAR,"+login+":/054048h6011.224N/02450.28E_c212s003g006t053h82b10046",
				login+">RXTLM-1,qAR,"+login+":T#363,12.2,0.0,62.0,13.0,0.0,00000000"),
		})
	})

	t.Run("area in south-west", func(t *testing.T) {
		rx.SetFilter("a/-23.5679/-47.3288/-23.5688/-47.3278")
		isclient.RunSteps(t, tx, rx, []isclient.Step{
			drop("north dropped",
				"DR0P-15>APN390,qAR,PY2PE-1:!2334.10N/04719.70W# drop north",
				"PU2TFG-15>APN390,qAR,PY2PE-1:!2334.10S/04719.70W# pass"),
			drop("east dropped",
				"DR0P-15>APN390,qAR,PY2PE-1:!2334.10S/04719.70E# drop east",
				"PU2TFG-15>BEACON,qAR,PY2PE-1:Rede Brasileira de APRS - Aluminio/SP"),
		})
	})

	// Filter for range around my position, and transmit my position too. Also
	// set a filter for friend range (its position is already known from the
	// first test).
	t.Run("my position and friend range", func(t *testing.T) {
		rx.SetFilter("m/1 f/" + login + "/10")
		// The position packet gets no reply, so give it a moment to reach
		// the position cache.
		rx.Send(rxLogin + ">APRS:!//HC`TPmVvR<_") // tampere
		time.Sleep(300 * time.Millisecond)

		isclient.RunSteps(t, tx, rx, []isclient.Step{
			// close to the previous compressed position
			drop("far position dropped, close compressed passes",
				"DR0P-15>APN390,qAR,"+login+":!2334.10S/04719.70E#",
				"T3ST>APRS,qAR,"+login+":!//H:sTQ\";vJI_ test"),
			// OH2TA is close enough to the friend, OH2FOI is not.
			drop("far friend dropped, near friend passes",
				"OH2FOI>APRS,qAR,"+login+":!6013.90N/02500.05E-",
				"OH1UK>APRS,TCPIP*,qAC,T2FINLAND:;OH2TA    *101302z6014.89N/02447.00Ey"),
		})
	})

	// Reconnect and see if the m/ filter still works based on a cached
	// position in historydb.
	t.Run("cached position", func(t *testing.T) {
		tx.Close()
		tx = isclient.Connect(t, "localhost:55580", login)
		tx.SetFilter("m/1")

		// a new transmitter is needed
		const tx2Login = "TX2LOG"
		tx2 := isclient.Connect(t, "localhost:55581", tx2Login)

		isclient.RunSteps(t, tx2, tx, []isclient.Step{
			drop("far position dropped, close position passes",
				fmt.Sprintf("DR0P-15>APN390,qAR,%s:!2334.10S/04719.70E#", tx2Login),
				fmt.Sprintf("T3ST-1>APRS,qAR,%s:/054048h6011.24N/02450.18E_c212s003g006t053h82b10046", tx2Login)),
		})
	})

	s.Stop()
}
