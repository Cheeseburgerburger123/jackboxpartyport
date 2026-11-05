package flash.net {
    import flash.utils.ByteArray;

    // Jackbox: a minimal SecureSocket, so content that references it doesn't fail.
    // TLS itself is the frontend's job (port 443 is treated as TLS there).
    public class SecureSocket extends Socket {
        public function SecureSocket() {
            super();
        }

        public static function get isSupported():Boolean {
            return true;
        }

        public function get serverCertificateStatus():String {
            return "trusted";
        }

        public function get serverCertificate():* {
            return null;
        }

        public function addBinaryChainBuildingCertificate(certificate:ByteArray, trusted:Boolean):void {
        }
    }
}
