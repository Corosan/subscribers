# subscribers

A couple of trivial samples how to organize multithreaded delivery of
notifications to a set of subscribers. One can think it could be be a super
small brother of boost::signals2 library. To tell the truth, I really never
understood why that library is so intricate.

In former design cases a "notification" is just a call to previously provided
std::function<void()> functor. Any parameter handling stuff is not a purpose of
initial design. The only goal of it is to organize subscription/unsubscription
and notification delivery in multithreaded environment. The second sample allows
to unsubscribe a consumer while being called from the consumer's callback. Last
samples include support for notification parameters handing.
