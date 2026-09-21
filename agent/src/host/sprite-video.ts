import type { CredentialStore } from "@earendil-works/pi-ai";
import { FalSpriteVideoProvider } from "@entisium/devkit/sprite-animation/video-provider";

export function createHostSpriteVideo(credentials: CredentialStore) {
    return new FalSpriteVideoProvider(async () => {
        if (process.env.FAL_KEY?.trim()) return process.env.FAL_KEY.trim();
        const credential = await credentials.read("fal");
        return credential?.type === "api_key" ? credential.key : undefined;
    });
}
