// Copyright (C) Microsoft Corporation. All rights reserved.

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"runtime/debug"
	"strings"

	"github.com/sirupsen/logrus"
)

func main() {
	os.Exit(run(os.Stdin, os.Stdout, os.Stderr))
}

type normalizer func(context.Context, loadRequest) (*normalizedProject, *responseError)

func run(input io.Reader, output io.Writer, errorOutput io.Writer) int {
	return runWithNormalizer(input, output, errorOutput, normalizeRequest)
}

func runWithNormalizer(input io.Reader, output io.Writer, errorOutput io.Writer, normalize normalizer) (exitCode int) {
	defer func() {
		if recovered := recover(); recovered != nil {
			fmt.Fprintf(errorOutput, "compose parser panic: %v\n%s", recovered, debug.Stack())
			exitCode = writeResponse(
				output,
				errorOutput,
				loadResponse{
					SchemaVersion: protocolSchemaVersion,
					Parser:        currentParserInfo(),
					Error: &responseError{
						Code:    "internal.panic",
						Message: "the Compose parser failed unexpectedly",
					},
				},
				2)
		}
	}()

	request, err := decodeRequest(input)
	if err != nil {
		response := loadResponse{
			SchemaVersion: protocolSchemaVersion,
			Parser:        currentParserInfo(),
			Error: &responseError{
				Code:    "request.invalid",
				Message: err.Error(),
			},
		}
		return writeResponse(output, errorOutput, response, 1)
	}

	warningOutput, restoreLogging := captureComposeWarnings()
	defer restoreLogging()
	project, responseErr := normalize(context.Background(), request)

	response := loadResponse{
		SchemaVersion: protocolSchemaVersion,
		Parser:        currentParserInfo(),
		Project:       project,
		Diagnostics:   warningDiagnostics(warningOutput.String()),
		Error:         responseErr,
	}

	exitCode = 0
	if responseErr != nil {
		exitCode = 1
	}

	return writeResponse(output, errorOutput, response, exitCode)
}

func captureComposeWarnings() (*bytes.Buffer, func()) {
	logger := logrus.StandardLogger()
	previousOutput := logger.Out
	previousFormatter := logger.Formatter
	previousLevel := logger.Level

	output := &bytes.Buffer{}
	logger.SetOutput(output)
	logger.SetFormatter(&logrus.TextFormatter{
		DisableColors:    true,
		DisableTimestamp: true,
	})
	logger.SetLevel(logrus.WarnLevel)

	return output, func() {
		logger.SetOutput(previousOutput)
		logger.SetFormatter(previousFormatter)
		logger.SetLevel(previousLevel)
	}
}

func warningDiagnostics(output string) []diagnostic {
	seen := map[string]struct{}{}
	diagnostics := []diagnostic{}
	for line := range strings.Lines(output) {
		message := strings.TrimSpace(line)
		if message == "" {
			continue
		}

		if _, exists := seen[message]; exists {
			continue
		}
		seen[message] = struct{}{}
		diagnostics = append(diagnostics, diagnostic{
			Severity: "warning",
			Code:     "compose.warning",
			Message:  message,
		})
	}
	return diagnostics
}

func writeResponse(output io.Writer, errorOutput io.Writer, response loadResponse, exitCode int) int {
	encoder := json.NewEncoder(output)
	encoder.SetEscapeHTML(false)
	encoder.SetIndent("", "  ")
	if err := encoder.Encode(response); err != nil {
		fmt.Fprintf(errorOutput, "encoding response: %v\n", err)
		return 2
	}

	return exitCode
}
