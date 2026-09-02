// Copyright (C) Microsoft Corporation. All rights reserved.

package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"path/filepath"
	"runtime/debug"
	"strings"
)

const (
	protocolSchemaVersion = 1
	maxRequestBytes       = 16 * 1024 * 1024
	composeGoModule       = "github.com/compose-spec/compose-go/v2"
)

type composeFile struct {
	Path    string `json:"path"`
	Content string `json:"content"`
}

type loadRequest struct {
	SchemaVersion    int               `json:"schemaVersion"`
	ProjectName      string            `json:"projectName"`
	WorkingDirectory string            `json:"workingDirectory"`
	Files            []composeFile     `json:"files"`
	Environment      map[string]string `json:"environment,omitempty"`
	Profiles         []string          `json:"profiles,omitempty"`
}

type parserInfo struct {
	Name    string `json:"name"`
	Version string `json:"version"`
}

type diagnostic struct {
	Severity string `json:"severity"`
	Code     string `json:"code"`
	Message  string `json:"message"`
}

type normalizedProject struct {
	Name             string          `json:"name"`
	WorkingDirectory string          `json:"workingDirectory"`
	ComposeFiles     []string        `json:"composeFiles"`
	Model            json.RawMessage `json:"model"`
}

type responseError struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

func (e *responseError) Error() string {
	return e.Message
}

type loadResponse struct {
	SchemaVersion int                `json:"schemaVersion"`
	Parser        parserInfo         `json:"parser"`
	Project       *normalizedProject `json:"project,omitempty"`
	Diagnostics   []diagnostic       `json:"diagnostics,omitempty"`
	Error         *responseError     `json:"error,omitempty"`
}

func decodeRequest(reader io.Reader) (loadRequest, error) {
	data, err := io.ReadAll(io.LimitReader(reader, maxRequestBytes+1))
	if err != nil {
		return loadRequest{}, fmt.Errorf("reading request: %w", err)
	}

	if len(data) > maxRequestBytes {
		return loadRequest{}, fmt.Errorf("request exceeds the %d-byte limit", maxRequestBytes)
	}

	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()

	var request loadRequest
	if err := decoder.Decode(&request); err != nil {
		return loadRequest{}, fmt.Errorf("decoding request: %w", err)
	}

	var trailing any
	if err := decoder.Decode(&trailing); err != io.EOF {
		if err == nil {
			return loadRequest{}, fmt.Errorf("request contains multiple JSON values")
		}

		return loadRequest{}, fmt.Errorf("decoding trailing request data: %w", err)
	}

	if err := validateRequest(request); err != nil {
		return loadRequest{}, err
	}

	return request, nil
}

func validateRequest(request loadRequest) error {
	if request.SchemaVersion != protocolSchemaVersion {
		return fmt.Errorf(
			"unsupported schemaVersion %d; expected %d",
			request.SchemaVersion,
			protocolSchemaVersion)
	}

	if request.ProjectName == "" {
		return fmt.Errorf("projectName is required")
	}

	if request.WorkingDirectory == "" {
		return fmt.Errorf("workingDirectory is required")
	}

	if !filepath.IsAbs(request.WorkingDirectory) {
		return fmt.Errorf("workingDirectory must be absolute: %q", request.WorkingDirectory)
	}

	if strings.ContainsRune(request.WorkingDirectory, '\x00') {
		return fmt.Errorf("workingDirectory contains a null character")
	}

	if len(request.Files) == 0 {
		return fmt.Errorf("at least one Compose file is required")
	}

	paths := map[string]struct{}{}
	for index, file := range request.Files {
		if file.Path == "" {
			return fmt.Errorf("files[%d].path is required", index)
		}

		if !filepath.IsAbs(file.Path) {
			return fmt.Errorf("files[%d].path must be absolute: %q", index, file.Path)
		}

		if strings.ContainsRune(file.Path, '\x00') {
			return fmt.Errorf("files[%d].path contains a null character", index)
		}

		if file.Content == "" {
			return fmt.Errorf("files[%d].content is required", index)
		}

		normalizedPath := strings.ToLower(filepath.Clean(file.Path))
		if _, exists := paths[normalizedPath]; exists {
			return fmt.Errorf("duplicate Compose file path: %q", file.Path)
		}
		paths[normalizedPath] = struct{}{}
	}

	for index, profile := range request.Profiles {
		if profile == "" {
			return fmt.Errorf("profiles[%d] must not be empty", index)
		}
	}

	return nil
}

func currentParserInfo() parserInfo {
	return parserInfo{
		Name:    "compose-go",
		Version: moduleVersion(composeGoModule),
	}
}

func moduleVersion(modulePath string) string {
	buildInfo, ok := debug.ReadBuildInfo()
	if !ok {
		return "unknown"
	}

	for _, dependency := range buildInfo.Deps {
		if dependency.Path != modulePath {
			continue
		}

		if dependency.Replace != nil && dependency.Replace.Version != "" {
			return dependency.Replace.Version
		}

		return dependency.Version
	}

	return "unknown"
}
