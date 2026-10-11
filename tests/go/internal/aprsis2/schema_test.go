package aprsis2

import (
	"crypto/sha256"
	"fmt"
	"os"
	"path/filepath"
	"testing"
)

// TestGeneratedIsCurrent fails if src/aprsis2.proto has changed since the Go
// code was generated from it.
func TestGeneratedIsCurrent(t *testing.T) {
	schema, err := os.ReadFile(filepath.Join("..", "..", "..", "..", "src", "aprsis2.proto"))
	if err != nil {
		t.Fatalf("cannot read the schema: %v", err)
	}

	if got := fmt.Sprintf("%x", sha256.Sum256(schema)); got != SchemaSHA256 {
		t.Errorf("src/aprsis2.proto has changed since the Go code was generated; run go generate ./internal/aprsis2")
	}
}
