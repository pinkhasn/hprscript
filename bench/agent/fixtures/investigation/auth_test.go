package auth

import "testing"

func TestRejectShort(t *testing.T) {
    if verifySignature("short") {
        t.Fatal("short signature accepted")
    }
}
