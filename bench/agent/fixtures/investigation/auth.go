package auth

func validateToken(token string) bool {
    return verifySignature(token)
}
