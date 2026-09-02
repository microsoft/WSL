// Copyright (C) Microsoft Corporation. All rights reserved.

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/sirupsen/logrus"
)

func newTestRequest(t *testing.T, content string) loadRequest {
	t.Helper()

	workingDirectory := t.TempDir()
	return loadRequest{
		SchemaVersion:    protocolSchemaVersion,
		ProjectName:      "example",
		WorkingDirectory: workingDirectory,
		Files: []composeFile{
			{
				Path:    filepath.Join(workingDirectory, "compose.yaml"),
				Content: content,
			},
		},
	}
}

func TestNormalizeRequest(t *testing.T) {
	workingDirectory := t.TempDir()
	request := loadRequest{
		SchemaVersion:    protocolSchemaVersion,
		ProjectName:      "example",
		WorkingDirectory: workingDirectory,
		Environment: map[string]string{
			"TAG": "1.27",
		},
		Files: []composeFile{
			{
				Path: filepath.Join(workingDirectory, "compose.yaml"),
				Content: `
services:
  web:
    image: "nginx:${TAG}"
    ports:
      - "8080:80"
`,
			},
		},
	}

	project, responseErr := normalizeRequest(context.Background(), request)
	if responseErr != nil {
		t.Fatalf("normalizeRequest() error = %v", responseErr)
	}

	if project.Name != "example" {
		t.Fatalf("project.Name = %q, want %q", project.Name, "example")
	}

	if project.WorkingDirectory != workingDirectory {
		t.Fatalf(
			"project.WorkingDirectory = %q, want %q",
			project.WorkingDirectory,
			workingDirectory)
	}

	var model struct {
		Services map[string]struct {
			Image string `json:"image"`
			Ports []struct {
				Target    uint32 `json:"target"`
				Published string `json:"published"`
			} `json:"ports"`
		} `json:"services"`
	}
	if err := json.Unmarshal(project.Model, &model); err != nil {
		t.Fatalf("unmarshalling normalized model: %v", err)
	}

	web := model.Services["web"]
	if web.Image != "nginx:1.27" {
		t.Fatalf("web.Image = %q, want %q", web.Image, "nginx:1.27")
	}

	if len(web.Ports) != 1 || web.Ports[0].Target != 80 || web.Ports[0].Published != "8080" {
		t.Fatalf("web.Ports = %#v, want canonical port 8080:80", web.Ports)
	}
}

func TestNormalizeRequestDoesNotUseProcessEnvironment(t *testing.T) {
	const variable = "COMPOSE_GO_WRAPPER_AMBIENT_TEST"
	t.Setenv(variable, "ambient-value")

	request := newTestRequest(t, `
services:
  web:
    image: nginx
    environment:
      - COMPOSE_GO_WRAPPER_AMBIENT_TEST
`)

	project, responseErr := normalizeRequest(context.Background(), request)
	if responseErr != nil {
		t.Fatalf("normalizeRequest() error = %v", responseErr)
	}

	var model struct {
		Services map[string]struct {
			Environment map[string]*string `json:"environment"`
		} `json:"services"`
	}
	if err := json.Unmarshal(project.Model, &model); err != nil {
		t.Fatalf("unmarshalling normalized model: %v", err)
	}

	if value := model.Services["web"].Environment[variable]; value != nil {
		t.Fatalf("environment value = %q, want unresolved null", *value)
	}
}

func TestNormalizeRequestUsesSuppliedContent(t *testing.T) {
	request := newTestRequest(t, `
services:
  web:
    image: nginx:request
`)
	if err := os.WriteFile(
		request.Files[0].Path,
		[]byte("services:\n  web:\n    image: nginx:disk\n"),
		0o600); err != nil {
		t.Fatalf("writing conflicting Compose file: %v", err)
	}

	project, responseErr := normalizeRequest(context.Background(), request)
	if responseErr != nil {
		t.Fatalf("normalizeRequest() error = %v", responseErr)
	}

	var model struct {
		Services map[string]struct {
			Image string `json:"image"`
		} `json:"services"`
	}
	if err := json.Unmarshal(project.Model, &model); err != nil {
		t.Fatalf("unmarshalling normalized model: %v", err)
	}

	if image := model.Services["web"].Image; image != "nginx:request" {
		t.Fatalf("web.Image = %q, want request content", image)
	}
}

func TestNormalizeRequestMergesFiles(t *testing.T) {
	workingDirectory := t.TempDir()
	request := loadRequest{
		SchemaVersion:    protocolSchemaVersion,
		ProjectName:      "example",
		WorkingDirectory: workingDirectory,
		Files: []composeFile{
			{
				Path: filepath.Join(workingDirectory, "compose.yaml"),
				Content: `
services:
  web:
    image: nginx:1
`,
			},
			{
				Path: filepath.Join(workingDirectory, "compose.override.yaml"),
				Content: `
services:
  web:
    image: nginx:2
`,
			},
		},
	}

	project, responseErr := normalizeRequest(context.Background(), request)
	if responseErr != nil {
		t.Fatalf("normalizeRequest() error = %v", responseErr)
	}

	var model struct {
		Services map[string]struct {
			Image string `json:"image"`
		} `json:"services"`
	}
	if err := json.Unmarshal(project.Model, &model); err != nil {
		t.Fatalf("unmarshalling normalized model: %v", err)
	}

	if image := model.Services["web"].Image; image != "nginx:2" {
		t.Fatalf("web.Image = %q, want %q", image, "nginx:2")
	}
}

func TestNormalizeRequestRejectsUncapturedFiles(t *testing.T) {
	workingDirectory := t.TempDir()
	request := loadRequest{
		SchemaVersion:    protocolSchemaVersion,
		ProjectName:      "example",
		WorkingDirectory: workingDirectory,
		Files: []composeFile{
			{
				Path: filepath.Join(workingDirectory, "compose.yaml"),
				Content: `
services:
  web:
    image: nginx
    env_file:
      - path: .env
`,
			},
		},
	}

	_, responseErr := normalizeRequest(context.Background(), request)
	if responseErr == nil {
		t.Fatal("normalizeRequest() succeeded, want an uncaptured file reference error")
	}

	if responseErr.Code != "compose.uncaptured_file_reference" {
		t.Fatalf(
			"normalizeRequest() error code = %q, want %q",
			responseErr.Code,
			"compose.uncaptured_file_reference")
	}

	if !strings.Contains(responseErr.Message, "services.web.env_file") {
		t.Fatalf("normalizeRequest() error = %q, want env_file property path", responseErr.Message)
	}
}

func TestNormalizeRequestRejectsInclude(t *testing.T) {
	workingDirectory := t.TempDir()
	request := loadRequest{
		SchemaVersion:    protocolSchemaVersion,
		ProjectName:      "example",
		WorkingDirectory: workingDirectory,
		Files: []composeFile{
			{
				Path: filepath.Join(workingDirectory, "compose.yaml"),
				Content: `
include:
  - included.yaml
services:
  web:
    image: nginx
`,
			},
		},
	}

	_, responseErr := normalizeRequest(context.Background(), request)
	if responseErr == nil {
		t.Fatal("normalizeRequest() succeeded, want an uncaptured file reference error")
	}

	if responseErr.Code != "compose.uncaptured_file_reference" {
		t.Fatalf(
			"normalizeRequest() error code = %q, want %q",
			responseErr.Code,
			"compose.uncaptured_file_reference")
	}

	if !strings.Contains(responseErr.Message, "include") {
		t.Fatalf("normalizeRequest() error = %q, want include property path", responseErr.Message)
	}
}

func TestNormalizeRequestRejectsExternalExtends(t *testing.T) {
	workingDirectory := t.TempDir()
	request := loadRequest{
		SchemaVersion:    protocolSchemaVersion,
		ProjectName:      "example",
		WorkingDirectory: workingDirectory,
		Files: []composeFile{
			{
				Path: filepath.Join(workingDirectory, "compose.yaml"),
				Content: `
services:
  web:
    extends:
      file: common.yaml
      service: base
`,
			},
		},
	}

	_, responseErr := normalizeRequest(context.Background(), request)
	if responseErr == nil {
		t.Fatal("normalizeRequest() succeeded, want an uncaptured file reference error")
	}

	if responseErr.Code != "compose.uncaptured_file_reference" {
		t.Fatalf(
			"normalizeRequest() error code = %q, want %q",
			responseErr.Code,
			"compose.uncaptured_file_reference")
	}

	if !strings.Contains(responseErr.Message, "services.web.extends.file") {
		t.Fatalf("normalizeRequest() error = %q, want extends.file property path", responseErr.Message)
	}
}

func TestNormalizeRequestSupportsSameFileExtends(t *testing.T) {
	workingDirectory := t.TempDir()
	request := loadRequest{
		SchemaVersion:    protocolSchemaVersion,
		ProjectName:      "example",
		WorkingDirectory: workingDirectory,
		Files: []composeFile{
			{
				Path: filepath.Join(workingDirectory, "compose.yaml"),
				Content: `
services:
  base:
    image: nginx
    environment:
      MODE: production
  web:
    extends:
      service: base
`,
			},
		},
	}

	project, responseErr := normalizeRequest(context.Background(), request)
	if responseErr != nil {
		t.Fatalf("normalizeRequest() error = %v", responseErr)
	}

	var model struct {
		Services map[string]struct {
			Image       string            `json:"image"`
			Environment map[string]string `json:"environment"`
		} `json:"services"`
	}
	if err := json.Unmarshal(project.Model, &model); err != nil {
		t.Fatalf("unmarshalling normalized model: %v", err)
	}

	web := model.Services["web"]
	if web.Image != "nginx" || web.Environment["MODE"] != "production" {
		t.Fatalf("web = %#v, want inherited image and environment", web)
	}
}

func TestNormalizeRequestIgnoresInactiveProfileEnvFile(t *testing.T) {
	request := newTestRequest(t, `
services:
  web:
    image: nginx
  debug:
    image: nginx
    profiles:
      - debug
    env_file:
      - path: missing.env
`)

	project, responseErr := normalizeRequest(context.Background(), request)
	if responseErr != nil {
		t.Fatalf("normalizeRequest() error = %v", responseErr)
	}

	var model struct {
		Services map[string]any `json:"services"`
	}
	if err := json.Unmarshal(project.Model, &model); err != nil {
		t.Fatalf("unmarshalling normalized model: %v", err)
	}

	if _, exists := model.Services["debug"]; exists {
		t.Fatal("inactive debug service is present in normalized model")
	}
}

func TestDecodeRequestRejectsUnknownProperties(t *testing.T) {
	input := `{
  "schemaVersion": 1,
  "projectName": "example",
  "workingDirectory": "C:\\example",
  "files": [
    {
      "path": "C:\\example\\compose.yaml",
      "content": "services: {}"
    }
  ],
  "unexpected": true
}`

	_, err := decodeRequest(strings.NewReader(input))
	if err == nil {
		t.Fatal("decodeRequest() succeeded, want an unknown property error")
	}

	if !strings.Contains(err.Error(), `unknown field "unexpected"`) {
		t.Fatalf("decodeRequest() error = %q, want unknown field detail", err)
	}
}

func TestRunReturnsNormalizedProject(t *testing.T) {
	request := newTestRequest(t, `
services:
  web:
    image: nginx
`)
	input, err := json.Marshal(request)
	if err != nil {
		t.Fatalf("marshalling request: %v", err)
	}

	var output bytes.Buffer
	var errorOutput bytes.Buffer
	exitCode := run(bytes.NewReader(input), &output, &errorOutput)

	if exitCode != 0 {
		t.Fatalf("run() exit code = %d, want 0; response = %s", exitCode, output.String())
	}

	if errorOutput.Len() != 0 {
		t.Fatalf("run() stderr = %q, want empty", errorOutput.String())
	}

	var response loadResponse
	if err := json.Unmarshal(output.Bytes(), &response); err != nil {
		t.Fatalf("unmarshalling response: %v", err)
	}

	if response.Project == nil || response.Project.Name != "example" {
		t.Fatalf("run() project = %#v, want normalized example project", response.Project)
	}

	if response.Parser.Version != "v2.14.0" {
		t.Fatalf("run() parser version = %q, want v2.14.0", response.Parser.Version)
	}

	if len(response.Diagnostics) != 0 {
		t.Fatalf("run() diagnostics = %#v, want none", response.Diagnostics)
	}
}

func TestCaptureComposeWarnings(t *testing.T) {
	output, restoreLogging := captureComposeWarnings()
	logrus.Warn("test warning")
	logrus.Warn("test warning")
	restoreLogging()

	diagnostics := warningDiagnostics(output.String())
	if len(diagnostics) != 1 {
		t.Fatalf("warningDiagnostics() = %#v, want one deduplicated warning", diagnostics)
	}

	if diagnostics[0].Severity != "warning" ||
		diagnostics[0].Code != "compose.warning" ||
		!strings.Contains(diagnostics[0].Message, "test warning") {
		t.Fatalf("warningDiagnostics() = %#v, want structured warning", diagnostics)
	}
}

func TestRunRecoversParserPanic(t *testing.T) {
	request := newTestRequest(t, "services: {}")
	input, err := json.Marshal(request)
	if err != nil {
		t.Fatalf("marshalling request: %v", err)
	}

	var output bytes.Buffer
	var errorOutput bytes.Buffer
	exitCode := runWithNormalizer(
		bytes.NewReader(input),
		&output,
		&errorOutput,
		func(context.Context, loadRequest) (*normalizedProject, *responseError) {
			panic("test panic")
		})

	if exitCode != 2 {
		t.Fatalf("runWithNormalizer() exit code = %d, want 2", exitCode)
	}

	var response loadResponse
	if err := json.Unmarshal(output.Bytes(), &response); err != nil {
		t.Fatalf("unmarshalling response: %v", err)
	}

	if response.Error == nil || response.Error.Code != "internal.panic" {
		t.Fatalf("runWithNormalizer() error = %#v, want internal.panic", response.Error)
	}

	if !strings.Contains(errorOutput.String(), "test panic") {
		t.Fatalf("runWithNormalizer() stderr = %q, want panic detail", errorOutput.String())
	}
}

func TestRunReturnsStructuredError(t *testing.T) {
	var output bytes.Buffer
	var errorOutput bytes.Buffer
	exitCode := run(strings.NewReader(`{"schemaVersion": 2}`), &output, &errorOutput)

	if exitCode != 1 {
		t.Fatalf("run() exit code = %d, want 1", exitCode)
	}

	if errorOutput.Len() != 0 {
		t.Fatalf("run() stderr = %q, want empty", errorOutput.String())
	}

	var response loadResponse
	if err := json.Unmarshal(output.Bytes(), &response); err != nil {
		t.Fatalf("unmarshalling response: %v", err)
	}

	if response.Error == nil || response.Error.Code != "request.invalid" {
		t.Fatalf("run() error = %#v, want request.invalid", response.Error)
	}
}
