// Copyright (C) Microsoft Corporation. All rights reserved.

package main

import (
	"context"
	"fmt"
	"sort"
	"strings"

	"github.com/compose-spec/compose-go/v2/loader"
	"github.com/compose-spec/compose-go/v2/types"
)

func normalizeRequest(ctx context.Context, request loadRequest) (*normalizedProject, *responseError) {
	config := types.ConfigDetails{
		WorkingDir:  request.WorkingDirectory,
		Environment: types.Mapping(request.Environment),
		ConfigFiles: make([]types.ConfigFile, 0, len(request.Files)),
	}

	composeFiles := make([]string, 0, len(request.Files))
	for _, file := range request.Files {
		config.ConfigFiles = append(config.ConfigFiles, types.ConfigFile{
			Filename: file.Path,
			Content:  []byte(file.Content),
		})
		composeFiles = append(composeFiles, file.Path)
	}

	preflightModel, err := loader.LoadModelWithContext(
		ctx,
		config,
		baseLoadOptions(request),
		func(options *loader.Options) {
			options.SkipInclude = true
			options.SkipExtends = true
		})
	if err != nil {
		return nil, &responseError{
			Code:    "compose.load_failed",
			Message: err.Error(),
		}
	}

	if references := findUncapturedFileReferences(preflightModel, request.Profiles); len(references) > 0 {
		return nil, &responseError{
			Code: "compose.uncaptured_file_reference",
			Message: fmt.Sprintf(
				"the prototype request does not capture external file references: %s",
				strings.Join(references, ", ")),
		}
	}

	project, err := loader.LoadWithContext(ctx, config, baseLoadOptions(request))
	if err != nil {
		return nil, &responseError{
			Code:    "compose.load_failed",
			Message: err.Error(),
		}
	}

	model, err := project.MarshalJSON()
	if err != nil {
		return nil, &responseError{
			Code:    "compose.serialize_failed",
			Message: err.Error(),
		}
	}

	return &normalizedProject{
		Name:             project.Name,
		WorkingDirectory: project.WorkingDir,
		ComposeFiles:     composeFiles,
		Model:            model,
	}, nil
}

func baseLoadOptions(request loadRequest) func(*loader.Options) {
	return func(options *loader.Options) {
		options.SetProjectName(request.ProjectName, true)
		options.Profiles = append([]string(nil), request.Profiles...)
		options.SkipValidation = false
		options.SkipInterpolation = false
		options.SkipNormalization = false
		options.ResolvePaths = true
		options.SkipConsistencyCheck = false
		options.SkipResolveEnvironment = false
		options.SkipResolveLabels = false
	}
}

func findUncapturedFileReferences(model map[string]any, activeProfiles []string) []string {
	references := []string{}
	if value, exists := model["include"]; exists && !isEmptyValue(value) {
		references = append(references, "include")
	}

	services, ok := model["services"].(map[string]any)
	if !ok {
		return references
	}

	for serviceName, value := range services {
		service, ok := value.(map[string]any)
		if !ok {
			continue
		}

		if serviceProfileIsActive(service, activeProfiles) {
			for _, field := range []string{"env_file", "label_file"} {
				if fieldValue, exists := service[field]; exists && !isEmptyValue(fieldValue) {
					references = append(references, fmt.Sprintf("services.%s.%s", serviceName, field))
				}
			}
		}

		extends, ok := service["extends"].(map[string]any)
		if !ok {
			continue
		}

		if file, exists := extends["file"]; exists && !isEmptyValue(file) {
			references = append(references, fmt.Sprintf("services.%s.extends.file", serviceName))
		}
	}

	sort.Strings(references)
	return references
}

func serviceProfileIsActive(service map[string]any, activeProfiles []string) bool {
	profiles, ok := service["profiles"].([]any)
	if !ok || len(profiles) == 0 {
		return true
	}

	active := map[string]struct{}{}
	for _, profile := range activeProfiles {
		active[profile] = struct{}{}
	}

	if _, allProfiles := active["*"]; allProfiles {
		return true
	}

	for _, value := range profiles {
		profile, ok := value.(string)
		if !ok {
			continue
		}

		if _, enabled := active[profile]; enabled {
			return true
		}
	}

	return false
}

func isEmptyValue(value any) bool {
	switch typed := value.(type) {
	case nil:
		return true
	case string:
		return typed == ""
	case []any:
		return len(typed) == 0
	case map[string]any:
		return len(typed) == 0
	default:
		return false
	}
}
