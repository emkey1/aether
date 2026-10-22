mod ScoreTools {
    export const BaseScore: Int = 50;

    @pure
    export fn clampScore(score: Int) -> Int {
        if score > 100 {
            ret 100;
        }
        ret score;
    }
}
