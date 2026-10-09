class EvidenceError(Exception):
    def __init__(self, code, message, remediation="", retryable=False):
        super().__init__(message)
        self.code = code
        self.message = message
        self.remediation = remediation
        self.retryable = retryable

    def payload(self):
        return {
            "code": self.code,
            "message": self.message,
            "remediation": self.remediation,
            "retryable": self.retryable,
        }
