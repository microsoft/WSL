//go:build cgo

package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"bytes"
	"encoding/json"
	"text/template"
	"unsafe"
)

func toJSONString(v interface{}) (string, error) {
	b, err := json.Marshal(v)
	if err != nil {
		return "", err
	}

	return string(b), nil
}

// Return codes
// Those errors should be in sync with the ones defined in TemplateRenderer
const (
	Success              = 0
	Fail_NullPointer     = 1
	Fail_ParseJSON       = 2
	Fail_ParseTemplate   = 3
	Fail_ExecuteTemplate = 4
)

// TryRenderGoTemplate renders a Go template with the provided JSON data.
// Returns 1 on success, 0 on failure. On success, *output contains the rendered result.
// On failure, *output contains the error message. The caller must free *output with FreeGoString.
//
//export TryRenderGoTemplate
func TryRenderGoTemplate(templateStr *C.char, jsonData *C.char, output **C.char) C.int {
	if templateStr == nil || jsonData == nil || output == nil {
		return Fail_NullPointer
	}

	var data interface{}
	if err := json.Unmarshal([]byte(C.GoString(jsonData)), &data); err != nil {
		*output = C.CString(err.Error())
		return Fail_ParseJSON
	}

	funcMap := template.FuncMap{
		"json": toJSONString,
	}

	tmpl, err := template.New("gotemplate").Funcs(funcMap).Parse(C.GoString(templateStr))
	if err != nil {
		*output = C.CString(err.Error())
		return Fail_ParseTemplate
	}

	var result bytes.Buffer
	if err = tmpl.Execute(&result, data); err != nil {
		*output = C.CString(err.Error())
		return Fail_ExecuteTemplate
	}

	*output = C.CString(result.String())
	return Success
}

// FreeGoString frees a string allocated by TryRenderGoTemplate.
//
//export FreeGoString
func FreeGoString(ptr *C.char) {
	C.free(unsafe.Pointer(ptr))
}

func main() {
}
