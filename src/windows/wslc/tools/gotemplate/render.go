// +build cgo

package main

/*
#include <stdlib.h>
#include <string.h>

// Exported C functions
char* RenderGoTemplate(char* templateStr, char* jsonData);
void FreeMemory(char* ptr);
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

// RenderGoTemplate renders a Go template with the provided JSON data.
// The template string and JSON data are passed as C strings.
// Returns a C string with the rendered output that must be freed by the caller.
//
//export RenderGoTemplate
func RenderGoTemplate(templateStr *C.char, jsonData *C.char) *C.char {
       if templateStr == nil || jsonData == nil {
               return C.CString("error: null pointers provided")
       }

       // Convert C strings to Go strings
       goTemplate := C.GoString(templateStr)
       goJSON := C.GoString(jsonData)

       // Parse the JSON data into a generic interface
       var data interface{}
       err := json.Unmarshal([]byte(goJSON), &data)
       if err != nil {
               return C.CString("error: failed to parse JSON: " + err.Error())
       }

   // Parse the template
       funcMap := template.FuncMap{
               "json": toJSONString,
       }

       tmpl, err := template.New("container-list").Funcs(funcMap).Parse(goTemplate)
       if err != nil {
               return C.CString("error: failed to parse template: " + err.Error())
       }

       // Render the template
       var result bytes.Buffer
       err = tmpl.Execute(&result, data)
       if err != nil {
               return C.CString("error: failed to execute template: " + err.Error())
       }

       // Return the result as a C string
       return C.CString(result.String())
}

//export FreeMemory
func FreeMemory(ptr *C.char) {
       C.free(unsafe.Pointer(ptr))
}

func main() {
}