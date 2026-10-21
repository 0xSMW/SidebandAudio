import XCTest

@MainActor
final class PlayerUITests: XCTestCase {
    func testPlaybackComparisonAndAllSettings() throws {
        let app = XCUIApplication()
        app.launch()
        let play = app.buttons["playButton"]
        XCTAssertTrue(play.waitForExistence(timeout: 15))
        let enabled = NSPredicate(format: "enabled == true")
        expectation(for: enabled, evaluatedWith: play)
        waitForExpectations(timeout: 15)
        XCTAssertEqual(play.label, "Play")

        play.tap()
        XCTAssertTrue(app.buttons.matching(identifier: "playButton").firstMatch.waitForExistence(timeout: 3))
        let sourceTime = app.staticTexts["sourceTime"]
        expectation(for: NSPredicate(format: "label != %@", "Original audio position, 0 minutes, 0 seconds"), evaluatedWith: sourceTime)
        waitForExpectations(timeout: 10)
        play.tap()
        XCTAssertEqual(play.label, "Play")

        let mode = app.segmentedControls["modePicker"]
        mode.buttons["Original"].tap()
        XCTAssertEqual(play.label, "Play", "Mode changes must preserve paused intent")
        mode.buttons["Processed"].tap()
        XCTAssertEqual(play.label, "Play")

        let scroll = app.scrollViews["settingsScroll"]
        for _ in 0..<5 where !app.buttons["addBandButton"].isHittable { scroll.swipeUp() }
        let addBand = app.buttons["addBandButton"]
        XCTAssertTrue(addBand.isHittable)
        addBand.tap()
        XCTAssertTrue(app.staticTexts["1 of 12 bands"].waitForExistence(timeout: 3))
        for _ in 0..<3 where !app.staticTexts["Frequency"].isHittable { scroll.swipeUp() }
        XCTAssertTrue(app.staticTexts["Frequency"].exists)
        XCTAssertTrue(app.staticTexts["Q"].exists)
        XCTAssertTrue(app.staticTexts["Gain"].exists)
        XCTAssertTrue(play.isHittable, "Transport stays available while editing EQ")
        let attachment = XCTAttachment(screenshot: app.screenshot())
        attachment.name = "Equalizer and persistent player"
        attachment.lifetime = .keepAlways
        add(attachment)
    }

    func testOriginalPlaybackReachesEndAndReplays() throws {
        let app = XCUIApplication()
        app.launch()
        let play = app.buttons["playButton"]
        XCTAssertTrue(play.waitForExistence(timeout: 15))
        expectation(for: NSPredicate(format: "enabled == true"), evaluatedWith: play)
        waitForExpectations(timeout: 15)
        app.segmentedControls["modePicker"].buttons["Original"].tap()
        play.tap()
        XCTAssertEqual(play.label, "Pause")
        expectation(for: NSPredicate(format: "label == 'Play'"), evaluatedWith: play)
        waitForExpectations(timeout: 30)
        XCTAssertEqual(app.staticTexts["silenceSaved"].label, "Silence saved, 0 minutes, 0 seconds")
        play.tap()
        XCTAssertEqual(play.label, "Pause")
        play.tap()
        XCTAssertEqual(play.label, "Play")
    }

    func testHeardSavingsSurviveSeekAndRepeatedModeChanges() throws {
        let app = XCUIApplication()
        app.launch()
        let play = app.buttons["playButton"]
        XCTAssertTrue(play.waitForExistence(timeout: 15))
        expectation(for: NSPredicate(format: "enabled == true"), evaluatedWith: play)
        waitForExpectations(timeout: 15)
        play.tap()
        XCTAssertEqual(play.label, "Pause")
        expectation(for: NSPredicate(format: "label == 'Play'"), evaluatedWith: play)
        waitForExpectations(timeout: 30)
        let savings = app.staticTexts["silenceSaved"]
        XCTAssertEqual(savings.label, "Silence saved, 0 minutes, 1 seconds")
        app.buttons["gobackward.15"].tap()
        let modes = app.segmentedControls["modePicker"]
        for _ in 0..<3 {
            modes.buttons["Original"].tap()
            modes.buttons["Processed"].tap()
        }
        // Neither queued processing nor changing paths may credit unheard savings again.
        XCTAssertEqual(savings.label, "Silence saved, 0 minutes, 1 seconds")
        XCTAssertEqual(play.label, "Play")
    }
}
