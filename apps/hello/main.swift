import UIKit

func sysctlString(_ name: String) -> String {
    var size = 0
    guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return "?" }
    var buf = [CChar](repeating: 0, count: size)
    guard sysctlbyname(name, &buf, &size, nil, 0) == 0 else { return "?" }
    return String(cString: buf)
}

final class HelloViewController: UIViewController {
    private let counter = UILabel()
    private var taps = 0

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemIndigo

        let title = UILabel()
        title.text = "سلام از Redmi Note 8!"
        title.font = .boldSystemFont(ofSize: 30)

        let info = UILabel()
        info.numberOfLines = 0
        info.textAlignment = .center
        info.font = .systemFont(ofSize: 17)
        let device = UIDevice.current
        info.text = """
        Hello from ginkgo
        \(device.systemName) \(device.systemVersion)
        hw.machine: \(sysctlString("hw.machine"))
        kernel: \(sysctlString("kern.ostype")) \(sysctlString("kern.osrelease"))
        """

        let button = UIButton(type: .system)
        button.setTitle("Tap me", for: .normal)
        button.titleLabel?.font = .boldSystemFont(ofSize: 24)
        button.backgroundColor = .white
        button.setTitleColor(.systemIndigo, for: .normal)
        button.layer.cornerRadius = 14
        button.contentEdgeInsets = UIEdgeInsets(top: 12, left: 36, bottom: 12, right: 36)
        button.addTarget(self, action: #selector(tapped), for: .touchUpInside)

        counter.text = "taps: 0"
        counter.font = .monospacedDigitSystemFont(ofSize: 22, weight: .medium)

        for label in [title, info, counter] {
            label.textColor = .white
        }
        let stack = UIStackView(arrangedSubviews: [title, info, button, counter])
        stack.axis = .vertical
        stack.alignment = .center
        stack.spacing = 28
        stack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            stack.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            stack.leadingAnchor.constraint(greaterThanOrEqualTo: view.leadingAnchor, constant: 20),
        ])
    }

    @objc private func tapped() {
        taps += 1
        counter.text = "taps: \(taps)"
    }
}

final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?

    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        let window = UIWindow(frame: UIScreen.main.bounds)
        window.rootViewController = HelloViewController()
        window.makeKeyAndVisible()
        self.window = window
        return true
    }
}

UIApplicationMain(CommandLine.argc, CommandLine.unsafeArgv, nil, NSStringFromClass(AppDelegate.self))
