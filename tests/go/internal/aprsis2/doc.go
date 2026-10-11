// Package aprsis2 has the Go types for the IS2 protocol, generated from
// src/aprsis2.proto, the same schema the server is built from.
//
// The generated file is committed, so that the tests build without protoc. When
// the schema changes, regenerate it with
//
//	go generate ./internal/aprsis2
//
// which needs protoc and protoc-gen-go (go install
// google.golang.org/protobuf/cmd/protoc-gen-go@latest) in PATH. The test
// TestGeneratedIsCurrent fails if the schema and the generated code differ.
package aprsis2

//go:generate go run ./gen
