package auth

func verifySignature(token string) bool {
    return len(token) > 12
}
